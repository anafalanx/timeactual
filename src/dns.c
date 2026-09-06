// dns.c -- DNS-over-HTTPS resolver with local SPKI enrollment and
// randomized enrolled-resolver failover. See dns.h for design rationale.

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "dns.h"
#include "netutil.h"
#include "logbuf.h"
#include "clock.h"
#include "pinned_tls.h"
#include "pin_store.h"
#include "h2.h"

#include "mbedtls/ssl.h"

// ---------------------------------------------------------------------------
// Pinned resolver table
// ---------------------------------------------------------------------------
//
// Each resolver has hardcoded anycast IP(s) so bootstrap needs no DNS.
// These addresses are reachability hints, not trust anchors: first-run
// enrollment validates the resolver certificate through Windows/Web PKI
// and stores the leaf SPKI in the protected local pin store.
//
// The client speaks HTTP/2 (preferred via ALPN) and HTTP/1.1. Quad9 and
// Mullvad serve DoH over HTTP/2 only: over HTTP/1.1 both complete the TLS
// handshake and then refuse the request (Quad9 with "505 HTTP Version Not
// Supported", Mullvad by closing without a response), which is why an
// entry that only ever gets as far as the handshake must never be judged
// healthy by its pin log lines alone -- the resolve lines say "via" whom.

static const DnsResolver kResolvers[] = {
    {
        .hostname = "cloudflare-dns.com",
        .ip_primary = "1.1.1.1",
        .ip_secondary = "1.0.0.1",
        .ip6_primary = "2606:4700:4700::1111",
        .ip6_secondary = "2606:4700:4700::1001",
        .label = "cloudflare",
        .operator_family = "cloudflare",
    },

    {
        .hostname = "dns.quad9.net",
        .ip_primary = "9.9.9.9",
        .ip_secondary = "149.112.112.112",
        .ip6_primary = "2620:fe::fe",
        .ip6_secondary = "2620:fe::9",
        .label = "quad9",
        .operator_family = "quad9",
    },

    {
        .hostname = "dns.google",
        .ip_primary = "8.8.8.8",
        .ip_secondary = "8.8.4.4",
        .ip6_primary = "2001:4860:4860::8888",
        .ip6_secondary = "2001:4860:4860::8844",
        .label = "google",
        .operator_family = "google",
    },

    {
        .hostname = "dns.nextdns.io",
        .ip_primary = "45.90.28.0",
        .ip_secondary = NULL,
        .ip6_primary = "2a07:a8c0::",
        .ip6_secondary = NULL,
        .label = "nextdns",
        .operator_family = "nextdns",
    },

    {
        .hostname = "dns.mullvad.net",
        .ip_primary = "194.242.2.2",
        .ip_secondary = NULL,
        .ip6_primary = "2a07:e340::2",
        .ip6_secondary = NULL,
        .label = "mullvad",
        .operator_family = "mullvad",
    },
};

#define DNS_POOL_SIZE (sizeof kResolvers / sizeof kResolvers[0])

static_assert(DNS_POOL_SIZE >= 2,
              "DoH resolver pool must keep random-provider diversity");

const DnsResolver *Dns_Pool(size_t *out_len)
{
    if (out_len) *out_len = DNS_POOL_SIZE;
    return kResolvers;
}

size_t Dns_PickResolvers(const DnsResolver **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t enabled[DNS_POOL_SIZE];
    size_t n_enabled = DNS_POOL_SIZE;
    for (size_t i = 0; i < DNS_POOL_SIZE; i++) enabled[i] = i;
    if (n_want > n_enabled) n_want = n_enabled;

    for (size_t i = 0; i < n_want; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return i;   // partial; caller decides
        }
        size_t j = i + (size_t)(r % (uint32_t)(n_enabled - i));
        size_t tmp = enabled[i];
        enabled[i] = enabled[j];
        enabled[j] = tmp;
        out[i] = &kResolvers[enabled[i]];
    }
    return n_want;
}

// ===========================================================================
// DNS wire format -- RFC 1035
// ===========================================================================

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_TYPE_AAAA   28
#define DNS_CLASS_IN    1
#define DNS_HDR_LEN     12
#define DNS_MAX_NAME    255

// The outbound family that most recently reached a DoH resolver -- a
// cheap "which family does this network route?" signal. AF_INET until a
// bootstrap connects (so IPv4/dual-stack networks keep IPv4-first
// behavior); flips to AF_INET6 only on an IPv6-only network where the
// v4 anycast addresses are unreachable. Read/written with plain int
// access -- it is a preference hint, never a correctness gate.
static volatile LONG g_preferred_family = AF_INET;

// ---------------------------------------------------------------------------
// Per-resolver runtime state
// ---------------------------------------------------------------------------
//
// Three things the pool needs to remember across queries, none persisted:
//
//  * Back-off. A resolver that fails DOH_BACKOFF_AFTER_FAILURES times in a
//    row (TLS rejected, unreachable, garbage) is skipped for DOH_BACKOFF_MS
//    instead of being re-tried -- and re-logged -- on every single lookup
//    of every cycle. Only transport, TLS, HTTP and malformed replies count:
//    a well-formed DNS answer of any kind (records, NODATA, NXDOMAIN, even
//    SERVFAIL) is a resolver in service. The AAAA NODATA every IPv4-only
//    NTP host produces must never look like a dead resolver. Nothing here
//    ever strands a lookup: when every resolver is backed off, the pool is
//    tried anyway.
//
//  * Out-of-window rotation candidates. Operators such as Google and Mullvad
//    rotate their DoH leaf keys long before the pinned certificate expires,
//    i.e. long before our renewal window opens. A CA-valid leaf that matches
//    no stored pin is therefore treated the way nts.c treats it: the answer
//    is USED (it is exactly as trustworthy as a first-run enrollment), but
//    the new key is persisted only once the SAME key is seen again at least
//    DOH_ROTATION_CORROBORATE_MS later. A single spoofed session cannot
//    enroll a key; DoH answers can in any case only steer us to NTP
//    addresses whose time must still pass the NTS-authenticated gate. Two
//    candidate slots per resolver cover anycast that alternates between
//    two POP keys.
//
//  * Log rate limits for the per-connection "local pin match" confirmation
//    (otherwise the single largest source of event-log volume) and for the
//    "CA validation accepted" line a pending rotation produces on every
//    connection until it is enrolled.
//
//  * HTTP/2 fallback. ALPN offers h2 first; if an HTTP/2 exchange with a
//    resolver ever fails at the protocol level, that resolver is spoken to
//    over HTTP/1.1 for the rest of the run (logged once) rather than
//    failing every query on a framing disagreement.
//
// Queries run on the worker threads, so the table sits behind its own CS.

#define DOH_BACKOFF_AFTER_FAILURES     3
#define DOH_BACKOFF_MS                 (60ULL * 60ULL * 1000ULL)   // 1 h
#define DOH_ROTATION_CORROBORATE_MS    (10ULL * 60ULL * 1000ULL)   // 10 min
#define DOH_PIN_MATCH_LOG_INTERVAL_MS  (60ULL * 60ULL * 1000ULL)   // 1 h
#define DOH_ROT_CANDIDATES             2

typedef struct {
    int      consecutiveFailures;
    uint64_t backoffUntilTick;      // 0 = not backed off
    uint64_t lastMatchLogTick;      // 0 = never logged this run
    uint64_t lastCaLogTick;         // "CA validation accepted" rate limit
    char     lastCaSpkiHex[65];
    uint64_t lastDiagLogTick;       // non-200 HTTP status diagnostics
    int      h2Disabled;            // an HTTP/2 exchange failed: HTTP/1.1 only
    // Rotation candidates; a slot is live while firstTick != 0. Two slots
    // so a provider whose anycast alternates between two POP keys can
    // still corroborate either of them.
    struct {
        uint8_t  spki[32];
        uint64_t firstTick;
    } cand[DOH_ROT_CANDIDATES];
} DohResolverState;

static DohResolverState g_resState[DNS_POOL_SIZE];
static CRITICAL_SECTION g_resCs;
static INIT_ONCE        g_resOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK res_state_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_resCs);
    return TRUE;
}

static DohResolverState *res_state(const DnsResolver *r) {
    InitOnceExecuteOnce(&g_resOnce, res_state_init, NULL, NULL);
    size_t idx = (size_t)(r - kResolvers);
    if (idx >= DNS_POOL_SIZE) idx = 0;
    return &g_resState[idx];
}

// The pure decision core, tick-parameterised so tests can drive time.
enum { DOH_ROT_NEW = 0, DOH_ROT_PENDING = 1, DOH_ROT_PROMOTE = 2 };

// doh_query_one outcomes beyond 0 (records returned) and -1 (transport,
// TLS, HTTP or malformed reply -- a resolver failure): the resolver
// answered, it just had nothing usable for us.
enum { DOH_ANSWERED_EMPTY = 1,    // NODATA / NXDOMAIN: no such record
       DOH_ANSWERED_ERROR = 2 };  // SERVFAIL, REFUSED...: ask another resolver

static int doh_rotation_observe_locked(DohResolverState *st,
                                       const uint8_t spki[32],
                                       uint64_t nowTick,
                                       uint64_t *outAgeMs) {
    for (int i = 0; i < DOH_ROT_CANDIDATES; i++) {
        if (st->cand[i].firstTick == 0 ||
            memcmp(st->cand[i].spki, spki, 32) != 0) continue;
        uint64_t age = nowTick - st->cand[i].firstTick;
        if (outAgeMs) *outAgeMs = age;
        if (age >= DOH_ROTATION_CORROBORATE_MS) {
            st->cand[i].firstTick = 0;      // consumed by the promotion
            return DOH_ROT_PROMOTE;
        }
        return DOH_ROT_PENDING;
    }
    // A new key takes a free slot, else evicts the oldest candidate.
    int slot = 0;
    for (int i = 0; i < DOH_ROT_CANDIDATES; i++) {
        if (st->cand[i].firstTick == 0) { slot = i; break; }
        if (st->cand[i].firstTick < st->cand[slot].firstTick) slot = i;
    }
    memcpy(st->cand[slot].spki, spki, 32);
    st->cand[slot].firstTick = nowTick ? nowTick : 1;
    if (outAgeMs) *outAgeMs = 0;
    return DOH_ROT_NEW;
}

static int doh_note_failure_locked(DohResolverState *st, uint64_t nowTick) {
    if (++st->consecutiveFailures >= DOH_BACKOFF_AFTER_FAILURES) {
        st->consecutiveFailures = 0;
        st->backoffUntilTick = nowTick + DOH_BACKOFF_MS;
        return 1;
    }
    return 0;
}

static int doh_backed_off_locked(const DohResolverState *st, uint64_t nowTick) {
    return st->backoffUntilTick != 0 && nowTick < st->backoffUntilTick;
}

static int doh_resolver_backed_off(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    EnterCriticalSection(&g_resCs);
    int off = doh_backed_off_locked(st, GetTickCount64());
    LeaveCriticalSection(&g_resCs);
    return off;
}

static void doh_note_failure(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    int entered;
    EnterCriticalSection(&g_resCs);
    entered = doh_note_failure_locked(st, GetTickCount64());
    LeaveCriticalSection(&g_resCs);
    if (entered) {
        Log_Append("dns: %s backing off for %llu min after %d consecutive failures",
                   r->label, (unsigned long long)(DOH_BACKOFF_MS / 60000ULL),
                   DOH_BACKOFF_AFTER_FAILURES);
    }
}

static void doh_note_success(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    int recovered = 0;
    EnterCriticalSection(&g_resCs);
    st->consecutiveFailures = 0;
    if (st->backoffUntilTick != 0) { st->backoffUntilTick = 0; recovered = 1; }
    LeaveCriticalSection(&g_resCs);
    if (recovered) Log_Append("dns: %s back in service", r->label);
}

// "local pin match" is proof of the security posture, but per connection
// it is pure repetition: log it on the first match of the run and then at
// most hourly per resolver.
static void doh_log_pin_match(const DnsResolver *r, const PinRecord *pin,
                              const char *peer_spki_hex) {
    DohResolverState *st = res_state(r);
    uint64_t now = GetTickCount64();
    int emit = 0;
    EnterCriticalSection(&g_resCs);
    if (st->lastMatchLogTick == 0 ||
        now - st->lastMatchLogTick >= DOH_PIN_MATCH_LOG_INTERVAL_MS) {
        st->lastMatchLogTick = now ? now : 1;
        emit = 1;
    }
    LeaveCriticalSection(&g_resCs);
    if (emit) {
        Log_Append("dns: %s local pin match host=%s spki=%s (%u enrolled) newest valid=%s..%s nextCa=%s",
                   r->label, r->hostname, peer_spki_hex,
                   (unsigned)pin->spki_count,
                   pin->not_before, pin->not_after,
                   pin->renewal_due[0] ? pin->renewal_due : "unknown");
    }
}

// While a rotation candidate awaits corroboration the leaf matches no pin,
// so every connection takes the CA path and would log its acceptance. Log
// the first acceptance per resolver, any change of key, every non-accepted
// outcome, and otherwise at most hourly.
static int doh_should_log_ca(const DnsResolver *r, const char *spki_hex,
                             int accepted) {
    if (!accepted) return 1;
    DohResolverState *st = res_state(r);
    uint64_t now = GetTickCount64();
    const char *hex = spki_hex ? spki_hex : "";
    int emit = 0;
    EnterCriticalSection(&g_resCs);
    if (st->lastCaLogTick == 0 ||
        now - st->lastCaLogTick >= DOH_PIN_MATCH_LOG_INTERVAL_MS ||
        strcmp(st->lastCaSpkiHex, hex) != 0) {
        st->lastCaLogTick = now ? now : 1;
        _snprintf(st->lastCaSpkiHex, sizeof st->lastCaSpkiHex, "%s", hex);
        st->lastCaSpkiHex[sizeof st->lastCaSpkiHex - 1] = 0;
        emit = 1;
    }
    LeaveCriticalSection(&g_resCs);
    return emit;
}

// A non-200 HTTP status is otherwise invisible ("3 consecutive failures"
// says nothing about WHY): log the first per resolver, then hourly.
static int doh_should_log_diag(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    uint64_t now = GetTickCount64();
    int emit = 0;
    EnterCriticalSection(&g_resCs);
    if (st->lastDiagLogTick == 0 ||
        now - st->lastDiagLogTick >= DOH_PIN_MATCH_LOG_INTERVAL_MS) {
        st->lastDiagLogTick = now ? now : 1;
        emit = 1;
    }
    LeaveCriticalSection(&g_resCs);
    return emit;
}

static int doh_h2_disabled(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    EnterCriticalSection(&g_resCs);
    int off = st->h2Disabled;
    LeaveCriticalSection(&g_resCs);
    return off;
}

static void doh_h2_failed(const DnsResolver *r) {
    DohResolverState *st = res_state(r);
    int first;
    EnterCriticalSection(&g_resCs);
    first = !st->h2Disabled;
    st->h2Disabled = 1;
    LeaveCriticalSection(&g_resCs);
    if (first) {
        Log_Append("dns: %s HTTP/2 exchange failed; using HTTP/1.1 with it for the rest of this run",
                   r->label);
    }
}

// h2.c transport callbacks over the pinned TLS session.
static int doh_h2_read(void *ctx, uint8_t *buf, size_t len) {
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)ctx;
    for (;;) {
        int rd = mbedtls_ssl_read(ssl, buf, len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return rd;
    }
}

static int doh_h2_write(void *ctx, const uint8_t *buf, size_t len) {
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)ctx;
    for (;;) {
        int wr = mbedtls_ssl_write(ssl, buf, len);
        if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return wr;
    }
}

// An out-of-window, CA-valid rotation: record/advance the candidate and
// persist once corroborated by a second sighting >= 10 min later.
static void doh_rotation_candidate(const DnsResolver *r,
                                   const CertVerifyWinResult *cert,
                                   const PinRecord *pin) {
    DohResolverState *st = res_state(r);
    uint64_t ageMs = 0;
    int verdict;
    EnterCriticalSection(&g_resCs);
    verdict = doh_rotation_observe_locked(st, cert->spki_sha256,
                                          GetTickCount64(), &ageMs);
    LeaveCriticalSection(&g_resCs);
    switch (verdict) {
    case DOH_ROT_NEW:
        Log_Append("dns: %s pin ROTATION observed outside renewal window host=%s newSpki=%s storedNewest=%s; answers used, enrollment deferred until the same key is seen again in >= %llu min",
                   r->label, r->hostname, cert->spki_hex, pin->spki_hex,
                   (unsigned long long)(DOH_ROTATION_CORROBORATE_MS / 60000ULL));
        break;
    case DOH_ROT_PROMOTE:
        PinStore_SavePin(PIN_ENDPOINT_DOH, r->label, r->hostname, 443,
                         r->operator_family, cert->spki_sha256, cert->spki_hex,
                         cert->not_before, cert->not_after,
                         cert->not_before_unix, cert->not_after_unix,
                         "pin-rotation-corroborated");
        Log_Append("dns: %s PIN ROTATION ACCEPTED host=%s spki=%s valid=%s..%s (same CA-valid key seen again after %llu min)",
                   r->label, r->hostname, cert->spki_hex,
                   cert->not_before, cert->not_after,
                   (unsigned long long)(ageMs / 60000ULL));
        break;
    default:
        break;   // pending: quiet
    }
}

#ifdef LUNAR_TESTING
// Test hooks over the pure decision cores (no I/O, caller-supplied ticks).
int Dns_TestObserveRotation(size_t resolverIdx, const uint8_t spki[32],
                            uint64_t nowTick) {
    if (resolverIdx >= DNS_POOL_SIZE) return -1;
    res_state(&kResolvers[resolverIdx]);
    EnterCriticalSection(&g_resCs);
    int v = doh_rotation_observe_locked(&g_resState[resolverIdx], spki,
                                        nowTick, NULL);
    LeaveCriticalSection(&g_resCs);
    return v;
}
int Dns_TestNoteFailure(size_t resolverIdx, uint64_t nowTick) {
    if (resolverIdx >= DNS_POOL_SIZE) return -1;
    res_state(&kResolvers[resolverIdx]);
    EnterCriticalSection(&g_resCs);
    int entered = doh_note_failure_locked(&g_resState[resolverIdx], nowTick);
    LeaveCriticalSection(&g_resCs);
    return entered;
}
int Dns_TestBackedOff(size_t resolverIdx, uint64_t nowTick) {
    if (resolverIdx >= DNS_POOL_SIZE) return -1;
    res_state(&kResolvers[resolverIdx]);
    EnterCriticalSection(&g_resCs);
    int off = doh_backed_off_locked(&g_resState[resolverIdx], nowTick);
    LeaveCriticalSection(&g_resCs);
    return off;
}
void Dns_TestResetResolverState(void) {
    res_state(&kResolvers[0]);
    EnterCriticalSection(&g_resCs);
    memset(g_resState, 0, sizeof g_resState);
    LeaveCriticalSection(&g_resCs);
}
#endif

// Encode a hostname as a length-prefixed label sequence into buf.
// Returns bytes written (including the terminating 0), or 0 on error.
static size_t encode_qname(const char *host, uint8_t *buf, size_t cap)
{
    if (!host || !*host) return 0;
    size_t  out  = 0;
    size_t  llen = 0;
    size_t  llp  = 0;    // position of current label-length byte
    if (cap < 1) return 0;
    buf[out++] = 0;      // placeholder for first length

    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (llen == 0) return 0;             // empty label
            if (llen > 63) return 0;             // label too long
            buf[llp] = (uint8_t)llen;
            if (out >= cap) return 0;
            llp = out;
            buf[out++] = 0;
            llen = 0;
        } else {
            // Valid DNS label chars: letters, digits, hyphen, underscore.
            // Reject anything else to avoid injection tricks.
            if (!(isalnum(c) || c == '-' || c == '_')) return 0;
            if (llen >= 63) return 0;
            if (out >= cap) return 0;
            buf[out++] = (uint8_t)c;
            llen++;
        }
    }
    // Trailing dot is optional; if host didn't end in one, close the
    // final label.
    if (llen > 0) {
        if (llen > 63) return 0;
        buf[llp] = (uint8_t)llen;
        if (out >= cap) return 0;
        buf[out++] = 0;     // root label
    } else {
        // already terminated by the '.' handler; the placeholder slot
        // from the loop IS the terminating 0.
    }
    if (out > DNS_MAX_NAME) return 0;
    return out;
}

// Build a DNS query for host/<qtype>. Shared by the A and AAAA paths.
static int dns_build_query(const char *host, uint16_t id, uint16_t qtype,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!host || !out || !out_len) return -1;
    if (out_cap < DNS_HDR_LEN + 5) return -1;

    // Header: ID, flags (RD=1), QDCOUNT=1.
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)(id & 0xff);
    out[2] = 0x01;      // RD
    out[3] = 0x00;
    out[4] = 0x00; out[5] = 0x01;   // QDCOUNT = 1
    out[6] = 0x00; out[7] = 0x00;   // ANCOUNT
    out[8] = 0x00; out[9] = 0x00;   // NSCOUNT
    out[10]= 0x00; out[11]= 0x00;   // ARCOUNT

    size_t qn = encode_qname(host, out + DNS_HDR_LEN,
                             out_cap - DNS_HDR_LEN - 4);
    if (qn == 0) return -1;

    size_t p = DNS_HDR_LEN + qn;
    out[p++] = 0x00; out[p++] = (uint8_t)qtype;
    out[p++] = 0x00; out[p++] = DNS_CLASS_IN;

    *out_len = p;
    return 0;
}

int Dns_BuildQueryA(const char *host, uint16_t id,
                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    return dns_build_query(host, id, DNS_TYPE_A, out, out_cap, out_len);
}

// Decode a (possibly compressed) domain name starting at `off` into
// `out_name` (a NUL-terminated lowercased dotted string). Returns the
// offset JUST AFTER the name on success (or for compressed names,
// after the 2-byte pointer in the source). On error returns 0.
// `out_name` may be NULL if the caller only wants to advance past the
// name without decoding it.
static size_t decode_name(const uint8_t *in, size_t len, size_t off,
                          char *out_name, size_t out_cap)
{
    if (off >= len) return 0;
    size_t op        = 0;
    size_t cursor    = off;
    int    followed  = 0;
    size_t end_after = 0;    // offset just after the first label sequence
    int    depth     = 0;

    for (;;) {
        if (cursor >= len) return 0;
        if (depth++ > 32) return 0;   // anti-loop ceiling
        uint8_t b = in[cursor];
        if ((b & 0xC0) == 0xC0) {
            // Pointer: two-byte big-endian, lower 14 bits is offset.
            if (cursor + 1 >= len) return 0;
            size_t ptr = ((size_t)(b & 0x3F) << 8) | in[cursor + 1];
            if (!followed) {
                end_after = cursor + 2;
                followed  = 1;
            }
            if (ptr >= cursor) return 0;  // forward/self pointer -> reject
            cursor = ptr;
            continue;
        }
        if (b & 0xC0) return 0;          // reserved labels
        if (b == 0) {
            // End of name.
            if (!followed) end_after = cursor + 1;
            if (out_name && op < out_cap) out_name[op] = 0;
            else if (out_name) return 0;
            return end_after;
        }
        size_t llen = b;
        cursor++;
        if (cursor + llen > len) return 0;
        if (llen > 63) return 0;
        if (out_name) {
            if (op > 0) {
                if (op + 1 >= out_cap) return 0;
                out_name[op++] = '.';
            }
            if (op + llen >= out_cap) return 0;
            for (size_t i = 0; i < llen; i++) {
                unsigned char c = in[cursor + i];
                // Lowercase ASCII for case-insensitive compare.
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
                out_name[op++] = (char)c;
            }
        }
        cursor += llen;
    }
}

// Case-insensitive ASCII domain compare. Tolerates a trailing dot on
// either side.
static int domain_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    // Tolerate trailing dots
    if (*a == '.' && !*(a + 1)) a++;
    if (*b == '.' && !*(b + 1)) b++;
    return *a == 0 && *b == 0;
}

int Dns_ParseResponseA(const uint8_t *in, size_t in_len,
                       uint16_t expect_id, const char *expect_host,
                       char (*out_ips)[16], size_t ips_cap,
                       size_t *out_count,
                       uint32_t *out_min_ttl)
{
    if (!in || !out_ips || !out_count || !expect_host) return -1;
    if (in_len < DNS_HDR_LEN) return -1;

    uint16_t id      = ((uint16_t)in[0] << 8) | in[1];
    uint16_t flags   = ((uint16_t)in[2] << 8) | in[3];
    uint16_t qdcount = ((uint16_t)in[4] << 8) | in[5];
    uint16_t ancount = ((uint16_t)in[6] << 8) | in[7];

    if (id != expect_id)  return -3;
    if ((flags & 0x8000) == 0) return -1;   // not a response
    int rcode = flags & 0x000F;
    if (rcode != 0) return -2;
    if (qdcount != 1) return -1;

    // Decode the question name and verify it matches expect_host.
    char  qname[DNS_MAX_NAME + 1];
    size_t off = DNS_HDR_LEN;
    size_t after_name = decode_name(in, in_len, off,
                                    qname, sizeof qname);
    if (after_name == 0) return -1;
    if (after_name + 4 > in_len) return -1;
    uint16_t qtype  = ((uint16_t)in[after_name] << 8) | in[after_name + 1];
    uint16_t qclass = ((uint16_t)in[after_name + 2] << 8) | in[after_name + 3];
    if (qtype != DNS_TYPE_A || qclass != DNS_CLASS_IN) return -1;
    if (!domain_equal_ci(qname, expect_host)) return -3;
    off = after_name + 4;

    size_t   n = 0;
    uint32_t min_ttl = UINT32_MAX;
    for (uint16_t i = 0; i < ancount && n < ips_cap; i++) {
        char rname[DNS_MAX_NAME + 1];
        size_t after_rname = decode_name(in, in_len, off,
                                         rname, sizeof rname);
        if (after_rname == 0) return -1;
        if (after_rname + 10 > in_len) return -1;
        uint16_t rtype  = ((uint16_t)in[after_rname]     << 8) | in[after_rname + 1];
        uint16_t rclass = ((uint16_t)in[after_rname + 2] << 8) | in[after_rname + 3];
        uint32_t ttl    = ((uint32_t)in[after_rname + 4] << 24)
                        | ((uint32_t)in[after_rname + 5] << 16)
                        | ((uint32_t)in[after_rname + 6] << 8)
                        |  (uint32_t)in[after_rname + 7];
        uint16_t rdlen  = ((uint16_t)in[after_rname + 8] << 8) | in[after_rname + 9];
        size_t   rdoff  = after_rname + 10;
        if (rdoff + rdlen > in_len) return -1;

        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4) {
            _snprintf(out_ips[n], 16, "%u.%u.%u.%u",
                      in[rdoff], in[rdoff + 1],
                      in[rdoff + 2], in[rdoff + 3]);
            if (ttl < min_ttl) min_ttl = ttl;
            n++;
        }
        // CNAMEs we just skip; a normal resolver follows them and
        // includes the target A records in the same answer section,
        // which is what we care about. If there are ONLY CNAMEs and
        // no terminal As, n stays 0 and we'll report failure.
        off = rdoff + rdlen;
    }

    *out_count = n;
    if (out_min_ttl) *out_min_ttl = (min_ttl == UINT32_MAX) ? 0 : min_ttl;
    return 0;
}

// General A/AAAA parser: same wire walk as Dns_ParseResponseA, but for
// either record type, formatting addresses (via inet_ntop) into
// NET_IP_STRLEN-wide buffers. want_qtype is DNS_TYPE_A or DNS_TYPE_AAAA;
// the question's qtype must match. Return codes match Dns_ParseResponseA
// (0 ok, -1 malformed, -2 rcode!=0, -3 id/question mismatch).
int Dns_ParseResponse(const uint8_t *in, size_t in_len,
                      uint16_t expect_id, const char *expect_host,
                      uint16_t want_qtype,
                      char (*out_ips)[NET_IP_STRLEN], size_t ips_cap,
                      size_t *out_count, uint32_t *out_min_ttl)
{
    if (!in || !out_ips || !out_count || !expect_host) return -1;
    if (want_qtype != DNS_TYPE_A && want_qtype != DNS_TYPE_AAAA) return -1;
    if (in_len < DNS_HDR_LEN) return -1;

    const size_t want_rdlen = (want_qtype == DNS_TYPE_AAAA) ? 16 : 4;
    const int    want_af    = (want_qtype == DNS_TYPE_AAAA) ? AF_INET6 : AF_INET;

    uint16_t id      = ((uint16_t)in[0] << 8) | in[1];
    uint16_t flags   = ((uint16_t)in[2] << 8) | in[3];
    uint16_t qdcount = ((uint16_t)in[4] << 8) | in[5];
    uint16_t ancount = ((uint16_t)in[6] << 8) | in[7];

    if (id != expect_id)  return -3;
    if ((flags & 0x8000) == 0) return -1;
    int rcode = flags & 0x000F;
    if (rcode != 0) return -2;
    if (qdcount != 1) return -1;

    char   qname[DNS_MAX_NAME + 1];
    size_t off = DNS_HDR_LEN;
    size_t after_name = decode_name(in, in_len, off, qname, sizeof qname);
    if (after_name == 0) return -1;
    if (after_name + 4 > in_len) return -1;
    uint16_t qtype  = ((uint16_t)in[after_name] << 8) | in[after_name + 1];
    uint16_t qclass = ((uint16_t)in[after_name + 2] << 8) | in[after_name + 3];
    if (qtype != want_qtype || qclass != DNS_CLASS_IN) return -1;
    if (!domain_equal_ci(qname, expect_host)) return -3;
    off = after_name + 4;

    size_t   n = 0;
    uint32_t min_ttl = UINT32_MAX;
    for (uint16_t i = 0; i < ancount && n < ips_cap; i++) {
        char rname[DNS_MAX_NAME + 1];
        size_t after_rname = decode_name(in, in_len, off, rname, sizeof rname);
        if (after_rname == 0) return -1;
        if (after_rname + 10 > in_len) return -1;
        uint16_t rtype  = ((uint16_t)in[after_rname]     << 8) | in[after_rname + 1];
        uint16_t rclass = ((uint16_t)in[after_rname + 2] << 8) | in[after_rname + 3];
        uint32_t ttl    = ((uint32_t)in[after_rname + 4] << 24)
                        | ((uint32_t)in[after_rname + 5] << 16)
                        | ((uint32_t)in[after_rname + 6] << 8)
                        |  (uint32_t)in[after_rname + 7];
        uint16_t rdlen  = ((uint16_t)in[after_rname + 8] << 8) | in[after_rname + 9];
        size_t   rdoff  = after_rname + 10;
        if (rdoff + rdlen > in_len) return -1;

        if (rtype == want_qtype && rclass == DNS_CLASS_IN && rdlen == want_rdlen) {
            if (inet_ntop(want_af, (void *)(in + rdoff),
                          out_ips[n], NET_IP_STRLEN) != NULL) {
                if (ttl < min_ttl) min_ttl = ttl;
                n++;
            }
        }
        off = rdoff + rdlen;
    }

    *out_count = n;
    if (out_min_ttl) *out_min_ttl = (min_ttl == UINT32_MAX) ? 0 : min_ttl;
    return 0;
}

int Dns_Intersect(const char (*set_a)[16], size_t n_a,
                  const char (*set_b)[16], size_t n_b,
                  char out_ip[16])
{
    for (size_t i = 0; i < n_a; i++) {
        for (size_t j = 0; j < n_b; j++) {
            if (strcmp(set_a[i], set_b[j]) == 0) {
                _snprintf(out_ip, 16, "%s", set_a[i]);
                return 0;
            }
        }
    }
    return -1;
}

// ===========================================================================
// Cache
// ===========================================================================
//
// Small fixed array indexed linearly. 32 entries easily covers the
// 6-sources-per-cycle traffic with room for KE-server override
// hostnames. Eviction: oldest expiring slot wins on insert collision.

#define DNS_CACHE_N     32
// Resolved IPs are cached at least this long so a resolve is not repeated on
// every poll. The floor sits well above the relaxed 5-min poll ceiling so a
// steady-state cycle reuses the cached IP instead of doing a fresh pinned-DoH
// handshake each time. Safe against a rotated/dead IP because the NTS TCP
// connect path invalidates a host's entry on connect failure (Dns_Invalidate),
// and unauthenticated core SNTP survives one dead source on the 3/4 majority.
#define DNS_TTL_FLOOR   900U
#define DNS_TTL_CEIL    3600U

typedef struct {
    char     host[256];
    char     ip[NET_IP_STRLEN];
    int      family;                // AF_INET / AF_INET6
    uint64_t expires_ms_tickcount;  // GetTickCount64 basis
} DnsCacheEntry;

static CRITICAL_SECTION g_cache_cs;
static volatile LONG    g_cache_cs_ready = 0;
static DnsCacheEntry    g_cache[DNS_CACHE_N];

static void cache_cs_ensure(void)
{
    if (InterlockedCompareExchange(&g_cache_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cache_cs);
        g_cache_cs_ready = 2;
    }
    while (g_cache_cs_ready != 2) { Sleep(0); }
}

static int cache_lookup(const char *host, char *out_ip, int *out_family)
{
    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    int found = 0;
    uint64_t now = GetTickCount64();
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_cache[i].host[0] == 0) continue;
        if (now >= g_cache[i].expires_ms_tickcount) {
            // Lazy expiry: wipe and keep scanning.
            g_cache[i].host[0] = 0;
            continue;
        }
        if (strcmp(g_cache[i].host, host) == 0) {
            _snprintf(out_ip, NET_IP_STRLEN, "%s", g_cache[i].ip);
            if (out_family) *out_family = g_cache[i].family;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_cache_cs);
    return found;
}

static void cache_insert(const char *host, const char *ip, int family,
                         uint32_t ttl)
{
    if (ttl < DNS_TTL_FLOOR) ttl = DNS_TTL_FLOOR;
    if (ttl > DNS_TTL_CEIL)  ttl = DNS_TTL_CEIL;

    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    uint64_t now = GetTickCount64();
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_cache[i].host[0] && strcmp(g_cache[i].host, host) == 0) {
            slot = i; break;
        }
        uint64_t exp = g_cache[i].host[0] ? g_cache[i].expires_ms_tickcount : 0;
        if (exp < oldest) { oldest = exp; oldest_slot = i; }
    }
    if (slot < 0) slot = oldest_slot;
    _snprintf(g_cache[slot].host, sizeof g_cache[slot].host, "%s", host);
    _snprintf(g_cache[slot].ip,   sizeof g_cache[slot].ip,   "%s", ip);
    g_cache[slot].family = family;
    g_cache[slot].expires_ms_tickcount = now + (uint64_t)ttl * 1000ULL;
    LeaveCriticalSection(&g_cache_cs);
}

void Dns_CacheClear(void)
{
    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    memset(g_cache, 0, sizeof g_cache);
    LeaveCriticalSection(&g_cache_cs);
}

// Drop any cached entry for `host` so the next resolve does a fresh lookup.
// Called when a connection to the cached IP fails, so a rotated/dead address
// is not reused for the full TTL floor.
void Dns_Invalidate(const char *host)
{
    if (!host || !host[0]) return;
    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_cache[i].host[0] && strcmp(g_cache[i].host, host) == 0)
            g_cache[i].host[0] = 0;
    }
    LeaveCriticalSection(&g_cache_cs);
}

// ===========================================================================
// DoH transport
// ===========================================================================

#define DOH_CONNECT_TIMEOUT_MS   4000
#define DOH_IO_TIMEOUT_MS        5000
#define DOH_MAX_REPLY_BYTES      65536

// TCP connect to a literal IPv4 or IPv6 address:port with a hard
// timeout. DoH bootstrap needs this so we never depend on the OS
// resolver. On success, remembers the family that connected as the
// network's preferred outbound family.
static SOCKET tcp_connect_ip(const char *ip_str, uint16_t port)
{
    struct sockaddr_storage sa;
    int salen = 0;
    int fam = Net_ParseIp(ip_str, port, &sa, &salen);
    if (fam == AF_UNSPEC) return INVALID_SOCKET;

    SOCKET s = Net_ConnectStream(&sa, salen,
                                 DOH_CONNECT_TIMEOUT_MS, DOH_IO_TIMEOUT_MS);
    if (s != INVALID_SOCKET) {
        InterlockedExchange(&g_preferred_family, fam);
    }
    return s;
}

// Execute a single DoH POST /dns-query against one pinned resolver IP.
// Returns 0 on success with up to ips_cap A records in out_ips and
// the smallest TTL in out_min_ttl. On any failure (connect, TLS, pin
// mismatch, HTTP error, DNS error), returns -1.
static int doh_query_one(const DnsResolver *r, const char *ip_str,
                         const char *host, uint16_t qid, uint16_t qtype,
                         char (*out_ips)[NET_IP_STRLEN], size_t ips_cap,
                         size_t *out_count, uint32_t *out_min_ttl)
{
    int      rc = -1;
    PinnedTls tls;
    uint8_t *reply = NULL;
    PinnedTls_Init(&tls);

    SOCKET s = tcp_connect_ip(ip_str, 443);
    if (s == INVALID_SOCKET) return -1;

    PinRecord pin;
    int have_pin = PinStore_GetPin(PIN_ENDPOINT_DOH, r->label, r->hostname,
                                   443, &pin);
    // The endpoint's usable pins: every stored, un-expired SPKI (multi-POP
    // and overlapping-rotation providers legitimately present more than
    // one key); a leaf matching ANY of them is an enrolled pin.
    uint8_t pin_set[PIN_STORE_MAX_SPKIS][32];
    size_t n_pins = have_pin ? PinStore_CollectValidSpkis(&pin, pin_set) : 0;
    int usable = n_pins > 0;
    int renew_due = usable ? PinStore_ShouldRenew(&pin) : 1;
    if (!have_pin) {
        Log_Append("dns: %s first-run enrollment required for %s:%u",
                   r->label, r->hostname, 443u);
    } else if (!usable) {
        Log_Append("dns: %s all %u local pin(s) expired; CA revalidation required (newest valid=%s..%s nextCa=%s)",
                   r->label, (unsigned)pin.spki_count,
                   pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown");
    } else if (renew_due) {
        Log_Append("dns: %s scheduled CA renewal due (pins=%u newest valid=%s..%s nextCa=%s nextCaUnix=%lld)",
                   r->label, (unsigned)n_pins, pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown",
                   (long long)pin.renewal_due_unix);
    }

    // CA fallback stays enabled even for an out-of-window mismatch (the
    // nts.c policy): an early key rotation is CA-validated and, if valid,
    // used as a rotation candidate instead of being hard-rejected until the
    // pinned certificate's own renewal window opens -- which for operators
    // that rotate early could mean weeks of a dead resolver.
    static const char *alpn_both[] = { "h2", "http/1.1", NULL };
    static const char *alpn_h1[]   = { "http/1.1", NULL };
    const char **alpn_list = doh_h2_disabled(r) ? alpn_h1 : alpn_both;
    PinnedTlsOpenResult openInfo;
    if (PinnedTls_OpenEnrolledSet(&tls, s, r->hostname, alpn_list,
                                  usable ? (const uint8_t (*)[32])pin_set : NULL,
                                  n_pins, 1 /* allow CA */,
                                  usable && renew_due, &openInfo) != 0) {
        if (openInfo.ca_attempted) {
            Log_Append("dns: %s CA validation rejected host=%s%s chain=0x%lx policy=0x%lx revocation=%s subject=\"%s\" issuer=\"%s\" spki=%s",
                       r->label, r->hostname,
                       openInfo.pin_mismatched
                           ? " (pin mismatch; unvalidated rotation refused)" : "",
                       (unsigned long)openInfo.cert.chain_error_status,
                       (unsigned long)openInfo.cert.policy_error,
                       openInfo.cert.revocation_offline ? "offline" :
                       (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                       openInfo.cert.subject, openInfo.cert.issuer,
                       openInfo.cert.spki_hex);
        }
        goto cleanup;
    }
    const char *neg = PinnedTls_NegotiatedAlpn(&tls);
    if (openInfo.ca_attempted) {
        if (doh_should_log_ca(r, openInfo.cert.spki_hex, openInfo.ca_valid))
        Log_Append("dns: %s CA validation %s host=%s alpn=%s subject=\"%s\" issuer=\"%s\" notBefore=%s notAfter=%s spki=%s revocation=%s chain=0x%lx policy=0x%lx",
                   r->label, openInfo.ca_valid ? "accepted" : "failed-but-pin-still-valid",
                   r->hostname, neg ? neg : "(none)",
                   openInfo.cert.subject, openInfo.cert.issuer,
                   openInfo.cert.not_before, openInfo.cert.not_after,
                   openInfo.cert.spki_hex,
                   openInfo.cert.revocation_offline ? "offline" :
                   (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                   (unsigned long)openInfo.cert.chain_error_status,
                   (unsigned long)openInfo.cert.policy_error);
        if (openInfo.ca_valid) {
            if (usable && !openInfo.pin_matched && !renew_due) {
                // Early/emergency rotation: use this session's answer, but
                // enroll the key only on a corroborating second sighting.
                doh_rotation_candidate(r, &openInfo.cert, &pin);
            } else {
                const char *status = !usable
                    ? (have_pin ? "expired-renewal" : "first-run-enrollment")
                    : (openInfo.pin_matched ? "scheduled-renewal" : "pin-rotation");
                PinStore_SavePin(PIN_ENDPOINT_DOH, r->label, r->hostname, 443,
                                 r->operator_family,
                                 openInfo.cert.spki_sha256,
                                 openInfo.cert.spki_hex,
                                 openInfo.cert.not_before,
                                 openInfo.cert.not_after,
                                 openInfo.cert.not_before_unix,
                                 openInfo.cert.not_after_unix,
                                 status);
            }
        }
    } else if (openInfo.pin_matched) {
        doh_log_pin_match(r, &pin, openInfo.peer_spki_hex);
    }
    mbedtls_ssl_context *ssl = PinnedTls_Ssl(&tls);

    // Build DNS query (A or AAAA).
    uint8_t dns_q[512];
    size_t  dns_q_len = 0;
    if (dns_build_query(host, qid, qtype, dns_q, sizeof dns_q, &dns_q_len) != 0)
        goto cleanup;

    reply = (uint8_t *)malloc(DOH_MAX_REPLY_BYTES);
    if (!reply) goto cleanup;
    const uint8_t *body = NULL;
    size_t body_len = 0;

    if (neg && strcmp(neg, "h2") == 0) {
        // HTTP/2: one POST on stream 1 (h2.c); the body lands in `reply`.
        H2Io io = { doh_h2_read, doh_h2_write, ssl };
        int status = 0;
        if (H2_PostOnce(&io, r->hostname, "/dns-query", "application/dns-message",
                        dns_q, dns_q_len, reply, DOH_MAX_REPLY_BYTES,
                        &body_len, &status) != 0) {
            doh_h2_failed(r);
            goto cleanup;
        }
        if (status != 200) {
            if (doh_should_log_diag(r)) {
                Log_Append("dns: %s host=%s answered HTTP/2 status %d (not 200); counted as a resolver failure",
                           r->label, r->hostname, status);
            }
            goto cleanup;
        }
        body = reply;
    } else {
        // HTTP/1.1: POST /dns-query, Connection: close, body runs to EOF.
        char req_hdr[512];
        int  hlen = _snprintf(req_hdr, sizeof req_hdr,
            "POST /dns-query HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/dns-message\r\n"
            "Content-Type: application/dns-message\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            r->hostname, (unsigned)dns_q_len);
        if (hlen <= 0 || (size_t)hlen >= sizeof req_hdr) goto cleanup;

        {
            size_t sent = 0;
            while (sent < (size_t)hlen) {
                int wr = mbedtls_ssl_write(ssl, (const unsigned char *)req_hdr + sent,
                                           (size_t)hlen - sent);
                if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (wr <= 0) goto cleanup;
                sent += (size_t)wr;
            }
            sent = 0;
            while (sent < dns_q_len) {
                int wr = mbedtls_ssl_write(ssl, dns_q + sent, dns_q_len - sent);
                if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (wr <= 0) goto cleanup;
                sent += (size_t)wr;
            }
        }

        // Drain reply.
        size_t reply_len = 0;
        for (;;) {
            int rd = mbedtls_ssl_read(ssl, reply + reply_len,
                                      DOH_MAX_REPLY_BYTES - reply_len);
            if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
            if (rd <= 0) {
                if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
                goto cleanup;
            }
            reply_len += (size_t)rd;
            if (reply_len >= DOH_MAX_REPLY_BYTES) goto cleanup;
        }

        // Parse HTTP: status line, headers, body. We need Content-Length
        // or Connection:close + EOF; most DoH servers do the latter so we
        // simply locate the end of headers and take everything after.
        if (reply_len < 16) goto cleanup;
        if (memcmp(reply, "HTTP/1.1 200", 12) != 0 &&
            memcmp(reply, "HTTP/1.0 200", 12) != 0) {
            // Name the status: a server that only speaks HTTP/2 answers 505 to
            // every query and would otherwise hide behind the failure count.
            if (doh_should_log_diag(r)) {
                size_t sl = 0;
                while (sl < reply_len && sl < 48 &&
                       reply[sl] != '\r' && reply[sl] != '\n') sl++;
                Log_Append("dns: %s host=%s answered \"%.*s\" (not 200); counted as a resolver failure",
                           r->label, r->hostname, (int)sl, (const char *)reply);
            }
            goto cleanup;
        }

        // Hand-roll a search for "\r\n\r\n"; memmem isn't in msvcrt.
        size_t hdr_end = 0;
        for (size_t i = 0; i + 3 < reply_len; i++) {
            if (reply[i]=='\r' && reply[i+1]=='\n' &&
                reply[i+2]=='\r' && reply[i+3]=='\n') {
                hdr_end = i + 4; break;
            }
        }
        if (hdr_end == 0) goto cleanup;
        body = reply + hdr_end;
        body_len = reply_len - hdr_end;
    }
    if (body_len < DNS_HDR_LEN) goto cleanup;

    size_t   n = 0;
    uint32_t min_ttl = 0;
    int pr = Dns_ParseResponse(body, body_len, qid, host, qtype,
                               out_ips, ips_cap, &n, &min_ttl);
    int rcode = body[3] & 0x0F;
    if (pr == 0 && n > 0) {
        *out_count   = n;
        if (out_min_ttl) *out_min_ttl = min_ttl;
        rc = 0;
    } else if (pr == 0 || (pr == -2 && rcode == 3 /* NXDOMAIN */)) {
        // A well-formed "nothing here" (NODATA for this qtype, NXDOMAIN):
        // the resolver is in service and has spoken; neither its other
        // addresses nor the rest of the pool would say otherwise.
        rc = DOH_ANSWERED_EMPTY;
    } else if (pr == -2) {
        // SERVFAIL / REFUSED / ...: in service, but no answer from here.
        rc = DOH_ANSWERED_ERROR;
    } else {
        goto cleanup;   // malformed or cross-wired reply: a real failure
    }

    PinnedTls_CloseNotify(&tls);

cleanup:
    PinnedTls_Free(&tls);
    free(reply);
    return rc;
}

// Try the resolver's bootstrap IPs in turn until one answers. Both
// IPv4 and IPv6 anycast addresses are tried; the family that most
// recently worked is tried first (so an IPv6-only network doesn't eat
// a connect timeout on every IPv4 address first, and a v4/dual-stack
// network keeps trying IPv4 first). The same SPKI pin covers every
// address -- they share one anycast leaf cert.
static int doh_query_resolver(const DnsResolver *r, const char *host,
                              uint16_t qid, uint16_t qtype,
                              char (*out_ips)[NET_IP_STRLEN], size_t ips_cap,
                              size_t *out_count, uint32_t *out_min_ttl)
{
    const char *v4[] = { r->ip_primary, r->ip_secondary };
    const char *v6[] = { r->ip6_primary, r->ip6_secondary };
    int prefer6 = (InterlockedCompareExchange(&g_preferred_family, 0, 0)
                   == AF_INET6);

    const char **order[2];
    order[0] = prefer6 ? v6 : v4;
    order[1] = prefer6 ? v4 : v6;

    for (int grp = 0; grp < 2; grp++) {
        for (int i = 0; i < 2; i++) {
            const char *ip = order[grp][i];
            if (!ip) continue;
            int q = doh_query_one(r, ip, host, qid, qtype,
                                  out_ips, ips_cap, out_count, out_min_ttl);
            if (q >= 0) {
                // Any well-formed DNS answer -- records, NODATA, even
                // SERVFAIL -- is a resolver in service; only transport, TLS,
                // HTTP and malformed replies count toward the back-off.
                doh_note_success(r);
                return q;
            }
        }
    }
    doh_note_failure(r);
    return -1;
}

// ===========================================================================
// Dns_Resolve -- public orchestrator
// ===========================================================================

#define DNS_ANSWERS_MAX 16

// One family's worth of DoH resolution: shuffle the pinned pool and try
// each resolver until one returns a record of `qtype`. Writes the first
// address to out_ip and its TTL to *out_ttl. Returns 0 / -1.
static int doh_resolve_qtype(const char *host, uint16_t qtype,
                             char *out_ip, uint32_t *out_ttl,
                             const char **out_via)
{
    const DnsResolver *picked[DNS_POOL_SIZE] = { NULL };
    size_t got = Dns_PickResolvers(picked, DNS_POOL_SIZE);
    if (got < 1) {
        Log_Append("dns: cannot resolve %s -- no enabled DoH resolvers", host);
        return -1;
    }

    char ips[DNS_ANSWERS_MAX][NET_IP_STRLEN];
    // Two passes: healthy resolvers first, then -- only if every one of
    // them failed -- the backed-off ones too, so a back-off can never
    // strand a lookup.
    for (int pass = 0; pass < 2; pass++) {
        int skipped = 0;
        for (size_t i = 0; i < got; i++) {
            int off = doh_resolver_backed_off(picked[i]);
            if (pass == 0 && off) { skipped++; continue; }
            if (pass == 1 && !off) continue;
            // Fresh QID per DoH query. DoH doesn't require it (the TLS
            // channel already defeats off-path spoofing) but it costs
            // nothing and lets us reject cross-wired responses.
            uint16_t qid = 0;
            BCryptGenRandom(NULL, (PUCHAR)&qid, sizeof qid,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);

            size_t   n_ips = 0;
            uint32_t ttl   = 0;
            int q = doh_query_resolver(picked[i], host, qid, qtype,
                                       ips, DNS_ANSWERS_MAX, &n_ips, &ttl);
            if (q == 0 && n_ips > 0) {
                _snprintf(out_ip, NET_IP_STRLEN, "%s", ips[0]);
                if (out_ttl) *out_ttl = ttl;
                if (out_via) *out_via = picked[i]->label;
                return 0;
            }
            // A pinned resolver's "no such record" is as authoritative as
            // its positive answers: walking the rest of the pool for the
            // same NODATA costs a TLS handshake per resolver and finds
            // nothing (the caller moves on to the other address family).
            if (q == DOH_ANSWERED_EMPTY) return -1;
        }
        if (pass == 0 && skipped == 0) break;   // nothing left to try
    }
    return -1;
}

int Dns_ResolveEx(const char *host, char *out_ip, int *out_family)
{
    if (!host || !out_ip) return -1;
    size_t hlen = strlen(host);
    if (hlen == 0 || hlen > 255) return -1;

    // If the caller already handed us a literal IP address there is nothing
    // to resolve. This matters for NTS: an NTS-KE server may negotiate its
    // NTP server as a bare IP (netnod returns e.g. 194.58.207.80), and
    // DoH-querying an IP as a hostname fails slowly across every pinned
    // resolver (~a minute of A then AAAA sweeps) before the system fallback
    // parses it -- long enough to blow the whole sync cycle's budget.
    {
        struct sockaddr_storage sa;
        int salen = 0;
        int litfam = Net_ParseIp(host, 0, &sa, &salen);
        if (litfam != AF_UNSPEC) {
            if (hlen >= NET_IP_STRLEN) return -1;
            memcpy(out_ip, host, hlen + 1);
            if (out_family) *out_family = litfam;
            return 0;
        }
    }

    int cached_fam = AF_UNSPEC;
    if (cache_lookup(host, out_ip, &cached_fam)) {
        if (out_family) *out_family = cached_fam;
        return 0;
    }

    // Query the family that the local network is known to route first
    // (the one that last reached a resolver), the other as fallback.
    // A/AAAA are both fetched securely over DoH; the downstream trust
    // gate (NTS pins, AEAD SNTP, concurrence) still guards the result.
    int prefer6 = (InterlockedCompareExchange(&g_preferred_family, 0, 0)
                   == AF_INET6);
    uint16_t first_qt = prefer6 ? DNS_TYPE_AAAA : DNS_TYPE_A;
    int      first_af = prefer6 ? AF_INET6      : AF_INET;
    uint16_t next_qt  = prefer6 ? DNS_TYPE_A    : DNS_TYPE_AAAA;
    int      next_af  = prefer6 ? AF_INET       : AF_INET6;

    uint32_t ttl = 0;
    const char *via = "?";
    if (doh_resolve_qtype(host, first_qt, out_ip, &ttl, &via) == 0) {
        cache_insert(host, out_ip, first_af, ttl);
        if (out_family) *out_family = first_af;
        Log_Append("dns: resolve %s -> %s  (%s, ttl=%us, via %s)",
                   host, out_ip, first_af == AF_INET6 ? "AAAA" : "A",
                   (unsigned)ttl, via);
        return 0;
    }
    if (doh_resolve_qtype(host, next_qt, out_ip, &ttl, &via) == 0) {
        cache_insert(host, out_ip, next_af, ttl);
        if (out_family) *out_family = next_af;
        Log_Append("dns: resolve %s -> %s  (%s, ttl=%us, via %s)",
                   host, out_ip, next_af == AF_INET6 ? "AAAA" : "A",
                   (unsigned)ttl, via);
        return 0;
    }

    Log_Append("dns: resolve %s FAIL (no A/AAAA via any pinned DoH resolver)",
               host);
    return -1;
}

int Dns_Resolve(const char *host, char out_ip[16])
{
    char   ip[NET_IP_STRLEN];
    int    fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) return -1;
    if (fam != AF_INET) return -1;    // legacy IPv4-only contract
    _snprintf(out_ip, 16, "%s", ip);
    return 0;
}

int Dns_ResolveSystem(const char *host, char *out_ip, int *out_family)
{
    if (!host || !out_ip) return -1;

    // getaddrinfo -- the OS resolver. For NTS hostnames ONLY: the NTS-KE
    // TLS session is pinned, so a forged answer is caught at TLS, not
    // used. Never call this for unauthenticated core SNTP.
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;    // v4 or v6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;

    int rc = -1;
    // Prefer the family the network is known to route.
    int prefer6 = (InterlockedCompareExchange(&g_preferred_family, 0, 0)
                   == AF_INET6);
    int want = prefer6 ? AF_INET6 : AF_INET;
    for (int pass = 0; pass < 2 && rc != 0; pass++) {
        for (struct addrinfo *a = res; a; a = a->ai_next) {
            if (a->ai_family != want) continue;
            void *addr = (a->ai_family == AF_INET6)
                ? (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr
                : (void *)&((struct sockaddr_in  *)a->ai_addr)->sin_addr;
            if (inet_ntop(a->ai_family, addr, out_ip, NET_IP_STRLEN)) {
                if (out_family) *out_family = a->ai_family;
                rc = 0;
                break;
            }
        }
        want = (want == AF_INET6) ? AF_INET : AF_INET6;   // other family
    }
    freeaddrinfo(res);
    if (rc == 0) {
        Log_Append("dns: system-DNS fallback resolved %s -> %s", host, out_ip);
    }
    return rc;
}
