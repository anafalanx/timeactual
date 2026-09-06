// nts.c -- TLS 1.3 + NTS-KE transport with local SPKI enrollment.
//
// Flow per call:
//
//   1. Resolve host -> IPv4 through the enrolled DoH resolver.
//   2. TCP connect with a 5 s timeout.
//   3. TLS 1.3 handshake (shared pinned_tls helper) with ALPN "ntske/1".
//   4. Match the leaf against the endpoint's enrolled SPKI set, or
//      perform Windows CA enrollment/renewal. A CA-valid leaf that
//      matches no stored pin outside the renewal window completes the
//      exchange as a PENDING ROTATION (not persisted here; see ntp.c).
//   5. Send the fixed NTS-KE client request (NtsKe_BuildClientRequest).
//   6. Drain inbound TLS records until peer-close or full response is
//      in our accumulator; parse with NtsKe_ParseResponse.
//   7. Export C2S and S2C AEAD keys via the TLS exporter
//      (RFC 8446 §7.5, RFC 8915 §5.1).
//   8. Graceful TLS close_notify, close socket.
//
// We hold no long-lived TLS RNG state. pinned_tls.c feeds mbedTLS through
// a BCryptGenRandom callback, so parallel exchanges do not share a
// mutable DRBG. Each exchange builds a fresh SSL config + context + socket.

// Winsock2 must come before <windows.h> -- see ntp.c for the same pattern.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"

#include "nts.h"
#include "nts_ke.h"
#include "nts_ef.h"
#include "clock.h"
#include "dns.h"
#include "netutil.h"
#include "logbuf.h"
#include "pinned_tls.h"
#include "pin_store.h"

// ---------------------------------------------------------------------------
// Provider pool
// ---------------------------------------------------------------------------
//
// Lunar ships only endpoint metadata here: no provider SPKI pins, no
// signing keys, and no other provider cryptographic trust material.
// First-run enrollment and renewal capture SPKI pins into the protected
// local pin store after Windows/Web-PKI validation succeeds.
//
// Source list is deliberately small and operator-curated. NTS is
// still a young protocol and not every public NTP pool runs it.

static const NtsProvider kProviders[] = {
    { .host = "time.cloudflare.com",        .port = 0, .label = "cloudflare",        .operator_family = "cloudflare" },
    { .host = "nts.netnod.se",              .port = 0, .label = "netnod",            .operator_family = "netnod" },
    { .host = "sth1.nts.netnod.se",         .port = 0, .label = "netnod-sth1",       .operator_family = "netnod" },
    { .host = "sth2.nts.netnod.se",         .port = 0, .label = "netnod-sth2",       .operator_family = "netnod" },
    /* NOTE: ptbtime{1..4}.ptb.de KE + cookies work, but PTB silently
     * drops NTS-sized (>48 byte) UDP packets at the NTP endpoint, so
     * the authenticated round trip never completes. We keep PTB as a
     * plain-SNTP core source (see src/ntp.c) and use System76's US
     * nodes here for additional NTS diversity. Verified 2026-04-21 via
     * scripts/probe_nts.py on ptbtime{1..4}. */
    { .host = "ohio.time.system76.com",     .port = 0, .label = "system76-ohio",     .operator_family = "system76" },
    { .host = "virginia.time.system76.com", .port = 0, .label = "system76-virginia", .operator_family = "system76" },
    { .host = "oregon.time.system76.com",   .port = 0, .label = "system76-oregon",   .operator_family = "system76" },
    { .host = "paris.time.system76.com",    .port = 0, .label = "system76-paris",    .operator_family = "system76" },
    { .host = "nts.time.nl",                .port = 0, .label = "sidn",              .operator_family = "sidn" },
};

#define NTS_PROVIDER_COUNT (sizeof kProviders / sizeof kProviders[0])

static_assert(NTS_PROVIDER_COUNT >= 2,
              "NTS provider pool must support distinct authenticated anchors");

const NtsProvider *Nts_Pool(size_t *out_len)
{
    if (out_len) *out_len = NTS_PROVIDER_COUNT;
    return kProviders;
}

const NtsProvider *Nts_PickProvider(void)
{
    uint32_t r = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return NULL;
    }
    return &kProviders[r % NTS_PROVIDER_COUNT];
}

// Sticky provider set: reuse the same operator-diverse draw across consecutive
// healthy cycles so the providers' cookie jars stay warm (RFC 8915 reuse), and
// rotate periodically for diversity-over-time. Nts_PickProviders is called once
// per cycle from the single aggregator thread, so no lock is needed. Rotate on
// demand (Nts_ForceRepick) whenever the clock is not yet TRUST_OK -- acquiring,
// holdover, or reacquiring after suspend/resume -- so re-anchor never rides a
// possibly-bad or stale pair.
#define NTS_STICKY_ROTATE_CYCLES 8
static const NtsProvider *g_sticky[NTS_PROVIDER_COUNT];
static size_t             g_sticky_n   = 0;
static int                g_sticky_age = 0;

void Nts_ForceRepick(void) { g_sticky_n = 0; }

// Per-provider running RTT (ms, EMA 1/4; 0 = never measured). Written by
// the NTS worker threads, read by the aggregator's draw: LONG + Interlocked.
// Providers at or under NTS_NEAR_RTT_MS are drawn first, never-measured ones
// next (so every provider gets measured), known-far ones last. The
// operator-diversity rule still applies within that order, so a near
// family member represents its family whenever it answers.
#define NTS_NEAR_RTT_MS 100
static volatile LONG g_rttEma[NTS_PROVIDER_COUNT];

static size_t provider_index(const NtsProvider *p) {
    return (size_t)(p - kProviders);
}

void Nts_ReportRtt(const NtsProvider *p, uint32_t rttMs)
{
    if (!p) return;
    size_t idx = provider_index(p);
    if (idx >= NTS_PROVIDER_COUNT) return;
    LONG old = g_rttEma[idx];
    LONG upd = old == 0 ? (LONG)rttMs : (LONG)((old * 3 + (LONG)rttMs) / 4);
    if (upd <= 0) upd = 1;   // 0 is reserved for "unknown"
    InterlockedExchange(&g_rttEma[idx], upd);
}

static int provider_tier(size_t idx) {
    LONG r = g_rttEma[idx];
    if (r == 0) return 1;                 // unmeasured: give it a turn
    return r <= NTS_NEAR_RTT_MS ? 0 : 2;  // near first, far last
}

#ifdef LUNAR_TESTING
uint32_t Nts_TestProviderRtt(size_t idx) {
    return idx < NTS_PROVIDER_COUNT ? (uint32_t)g_rttEma[idx] : 0;
}
void Nts_TestResetPicker(void) {
    for (size_t i = 0; i < NTS_PROVIDER_COUNT; i++) g_rttEma[i] = 0;
    g_sticky_n = 0;
    g_sticky_age = 0;
}
#endif

size_t Nts_PickProviders(const NtsProvider **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t enabled[NTS_PROVIDER_COUNT];
    size_t n_enabled = NTS_PROVIDER_COUNT;
    for (size_t i = 0; i < n_enabled; i++) enabled[i] = i;
    if (n_want > n_enabled) n_want = n_enabled;

    // Reuse the current sticky set if it matches this request and has not aged
    // out. This keeps handshakes rare in steady state instead of drawing fresh
    // (cold-jar) hosts every cycle.
    if (g_sticky_n == n_want && g_sticky_age < NTS_STICKY_ROTATE_CYCLES) {
        for (size_t i = 0; i < n_want; i++) out[i] = g_sticky[i];
        g_sticky_age++;
        return n_want;
    }

    // Fisher-Yates shuffle over the metadata indices using
    // BCryptGenRandom for each draw. n is small (<= pool size), so
    // this is trivially constant-work and uniform.
    for (size_t i = 0; i < n_enabled; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return 0;
        }
        size_t j = i + (size_t)(r % (uint32_t)(n_enabled - i));
        size_t tmp = enabled[i];
        enabled[i] = enabled[j];
        enabled[j] = tmp;
    }

    // Stable partition of the shuffled order by RTT tier: the draw stays
    // uniformly random WITHIN a tier, but near providers come before
    // unmeasured ones, which come before known-far ones.
    {
        size_t ordered[NTS_PROVIDER_COUNT];
        size_t n_ordered = 0;
        for (int tier = 0; tier <= 2; tier++) {
            for (size_t i = 0; i < n_enabled; i++) {
                if (provider_tier(enabled[i]) == tier) ordered[n_ordered++] = enabled[i];
            }
        }
        for (size_t i = 0; i < n_enabled; i++) enabled[i] = ordered[i];
    }

    for (size_t i = 0; i < n_want; i++) out[i] = NULL;

    size_t written = 0;
    for (size_t i = 0; i < n_enabled && written < n_want; i++) {
        const NtsProvider *p = &kProviders[enabled[i]];
        const char *fam = p->operator_family ? p->operator_family : "";
        int duplicate_family = 0;
        for (size_t j = 0; j < written; j++) {
            const char *old = out[j] && out[j]->operator_family
                ? out[j]->operator_family : "";
            if (strcmp(fam, old) == 0) {
                duplicate_family = 1;
                break;
            }
        }
        if (!duplicate_family) out[written++] = p;
    }
    for (size_t i = 0; i < n_enabled && written < n_want; i++) {
        const NtsProvider *p = &kProviders[enabled[i]];
        int already = 0;
        for (size_t j = 0; j < written; j++) {
            if (out[j] == p) {
                already = 1;
                break;
            }
        }
        if (!already) out[written++] = p;
    }

    // Remember this draw as the new sticky set (only when it fully satisfied
    // the request, so a short/degraded draw is not pinned).
    if (written == n_want && written <= NTS_PROVIDER_COUNT) {
        for (size_t i = 0; i < written; i++) g_sticky[i] = out[i];
        g_sticky_n   = written;
        g_sticky_age = 0;
    } else {
        g_sticky_n = 0;
    }
    return written;
}

// ---------------------------------------------------------------------------
// TCP connect with a hard timeout
// ---------------------------------------------------------------------------

#define NTS_CONNECT_TIMEOUT_MS   5000
#define NTS_IO_TIMEOUT_MS        5000
#define NTS_MAX_REPLY_BYTES      16384

static SOCKET tcp_connect(const char *host, uint16_t port)
{
    // Resolve via pinned DoH first (dual-stack A/AAAA). If every pinned
    // resolver is blocked -- common on corporate networks that force
    // their own DNS and firewall 443 to public resolvers -- fall back
    // to the OS resolver. That fallback is safe HERE and only here: the
    // NTS-KE session below is authenticated by a locally enrolled SPKI
    // pin, so a forged system-DNS answer cannot redirect us to a host
    // whose leaf an attacker controls -- it fails at TLS. (Core SNTP,
    // which is unauthenticated, must never do this.)
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) {
        if (Dns_ResolveSystem(host, ip, &fam) != 0) return INVALID_SOCKET;
    }

    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, port, &sa, &salen) == AF_UNSPEC) return INVALID_SOCKET;

    SOCKET s = Net_ConnectStream(&sa, salen,
                                 NTS_CONNECT_TIMEOUT_MS, NTS_IO_TIMEOUT_MS);
    if (s == INVALID_SOCKET) {
        // The address (possibly a long-cached DoH result) did not connect --
        // drop its cache entry so the next attempt re-resolves rather than
        // riding the full TTL floor on a rotated/dead IP.
        Dns_Invalidate(host);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Nts_DoKe
// ---------------------------------------------------------------------------

int Nts_DoKe(const NtsProvider *p, NtsKeResult *out)
{
    return Nts_DoKeEx(p, out, NULL);
}

int Nts_DoKeEx(const NtsProvider *p, NtsKeResult *out,
               NtsRotationPending *rot)
{
    if (rot) memset(rot, 0, sizeof *rot);
    if (p == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    int      rc = -1;
    PinnedTls tls;
    PinnedTls_Init(&tls);

    uint16_t port = p->port ? p->port : 4460;
    SOCKET s = tcp_connect(p->host, port);
    if (s == INVALID_SOCKET) goto cleanup;

    // ALPN: exactly "ntske/1" (RFC 8915 §4).
    static const char *alpn_list[] = { "ntske/1", NULL };
    PinRecord pin;
    int have_pin = PinStore_GetPin(PIN_ENDPOINT_NTS, p->label, p->host,
                                   port, &pin);
    // The endpoint's usable pins: every stored, un-expired SPKI. A leaf
    // matching ANY of them authenticates as an enrolled pin.
    uint8_t pin_set[PIN_STORE_MAX_SPKIS][32];
    size_t n_pins = have_pin ? PinStore_CollectValidSpkis(&pin, pin_set) : 0;
    int usable = n_pins > 0;
    int renew_due = usable ? PinStore_ShouldRenew(&pin) : 1;
    if (!have_pin) {
        Log_Append("nts: %s first-run enrollment required for %s:%u",
                   p->label, p->host, (unsigned)port);
    } else if (!usable) {
        Log_Append("nts: %s all %u local pin(s) expired; CA revalidation required (newest valid=%s..%s nextCa=%s)",
                   p->label, (unsigned)pin.spki_count,
                   pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown");
    } else if (renew_due) {
        Log_Append("nts: %s scheduled CA renewal due (pins=%u newest valid=%s..%s nextCa=%s nextCaUnix=%lld)",
                   p->label, (unsigned)n_pins,
                   pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown",
                   (long long)pin.renewal_due_unix);
    }

    // CA fallback stays enabled even for an out-of-window mismatch:
    // instead of hard-rejecting an early/emergency key rotation, the
    // leaf is CA-validated and, if valid, flagged as a pending rotation
    // for the aggregator to corroborate (never persisted here).
    PinnedTlsOpenResult openInfo;
    if (PinnedTls_OpenEnrolledSet(&tls, s, p->host, alpn_list,
                                  usable ? (const uint8_t (*)[32])pin_set : NULL,
                                  n_pins, 1 /* allow CA */,
                                  usable && renew_due, &openInfo) != 0) {
        if (openInfo.ca_attempted) {
            Log_Append("nts: %s CA validation rejected host=%s%s chain=0x%lx policy=0x%lx revocation=%s subject=\"%s\" issuer=\"%s\" spki=%s",
                       p->label, p->host,
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
    mbedtls_ssl_context *ssl = PinnedTls_Ssl(&tls);

    // ALPN check.
    {
        const char *neg = PinnedTls_NegotiatedAlpn(&tls);
        if (neg == NULL || strcmp(neg, "ntske/1") != 0) goto cleanup;
        if (openInfo.ca_attempted) {
            Log_Append("nts: %s CA validation %s host=%s alpn=%s subject=\"%s\" issuer=\"%s\" notBefore=%s notAfter=%s spki=%s revocation=%s chain=0x%lx policy=0x%lx",
                       p->label,
                       openInfo.ca_valid ? "accepted" : "failed-but-pin-still-valid",
                       p->host, neg,
                       openInfo.cert.subject, openInfo.cert.issuer,
                       openInfo.cert.not_before, openInfo.cert.not_after,
                       openInfo.cert.spki_hex,
                       openInfo.cert.revocation_offline ? "offline" :
                       (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                       (unsigned long)openInfo.cert.chain_error_status,
                       (unsigned long)openInfo.cert.policy_error);
            if (openInfo.ca_valid) {
                if (usable && !openInfo.pin_matched && !renew_due) {
                    // Early/emergency rotation: CA-valid leaf, no pin
                    // match, outside the renewal window. Complete the
                    // exchange but DO NOT persist; the aggregator
                    // promotes the pin only after an operator-diverse,
                    // still-pinned peer corroborates the cycle.
                    if (rot) {
                        rot->pending = 1;
                        rot->port = port;
                        memcpy(rot->spki, openInfo.cert.spki_sha256, 32);
                        memcpy(rot->spki_hex, openInfo.cert.spki_hex,
                               sizeof rot->spki_hex);
                        memcpy(rot->old_spki_hex, pin.spki_hex,
                               sizeof rot->old_spki_hex);
                        memcpy(rot->not_before, openInfo.cert.not_before,
                               sizeof rot->not_before);
                        memcpy(rot->not_after, openInfo.cert.not_after,
                               sizeof rot->not_after);
                        rot->not_before_unix = openInfo.cert.not_before_unix;
                        rot->not_after_unix  = openInfo.cert.not_after_unix;
                    }
                    Log_Append("nts: %s pin ROTATION observed outside renewal window host=%s newSpki=%s storedNewest=%s pins=%u; enrollment deferred pending operator-diverse corroboration",
                               p->label, p->host, openInfo.cert.spki_hex,
                               pin.spki_hex, (unsigned)n_pins);
                } else {
                    const char *status = !usable
                        ? (have_pin ? "expired-renewal" : "first-run-enrollment")
                        : (openInfo.pin_matched ? "scheduled-renewal"
                                                : "pin-rotation");
                    PinStore_SavePin(PIN_ENDPOINT_NTS, p->label, p->host, port,
                                     p->operator_family,
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
            // Proof of posture, but per-exchange it is pure repetition:
            // first match of the run, then at most hourly per provider.
            static volatile LONG64 s_lastMatchLog[NTS_PROVIDER_COUNT];
            size_t idx = provider_index(p);
            LONG64 now = (LONG64)GetTickCount64();
            LONG64 last = idx < NTS_PROVIDER_COUNT ? s_lastMatchLog[idx] : 0;
            if (idx >= NTS_PROVIDER_COUNT || last == 0 ||
                now - last >= 60LL * 60LL * 1000LL) {
                if (idx < NTS_PROVIDER_COUNT) {
                    InterlockedExchange64(&s_lastMatchLog[idx], now ? now : 1);
                }
                Log_Append("nts: %s local pin match host=%s spki=%s (1 of %u enrolled) newest valid=%s..%s nextCa=%s",
                           p->label, p->host, openInfo.peer_spki_hex,
                           (unsigned)n_pins,
                           pin.not_before, pin.not_after,
                           pin.renewal_due[0] ? pin.renewal_due : "unknown");
            }
        }
    }

    // Send NTS-KE request.
    {
        uint8_t req[32];
        size_t  rlen = NtsKe_BuildClientRequest(req, sizeof req);
        if (rlen == 0) goto cleanup;
        size_t sent = 0;
        while (sent < rlen) {
            int wr = mbedtls_ssl_write(ssl, req + sent, rlen - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
    }

    // Drain reply.
    uint8_t reply[NTS_MAX_REPLY_BYTES];
    size_t  reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(ssl, reply + reply_len,
                                  sizeof reply - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            // Servers sometimes drop TCP after the reply without
            // sending close_notify; treat a clean CONN_EOF or 0 as
            // end-of-message if we already accumulated something.
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            goto cleanup;
        }
        reply_len += (size_t)rd;
        if (reply_len >= sizeof reply) goto cleanup;  // ceiling hit
    }

    // Parse.
    NtsKeResponse resp;
    if (NtsKe_ParseResponse(reply, reply_len, &resp) != 1) goto cleanup;

    // Export AEAD keys per RFC 8915 §5.1.
    static const char kExporterLabel[] = "EXPORTER-network-time-security";
    {
        // Context for C2S: [proto u16][aead u16][S-bit u8]
        uint8_t ctx_c2s[5] = { 0x00, 0x00, 0x00, 0x0f, 0x00 };
        uint8_t ctx_s2c[5] = { 0x00, 0x00, 0x00, 0x0f, 0x01 };
        if (mbedtls_ssl_export_keying_material(ssl,
                out->c2s_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_c2s, sizeof ctx_c2s, 1) != 0) goto cleanup;
        if (mbedtls_ssl_export_keying_material(ssl,
                out->s2c_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_s2c, sizeof ctx_s2c, 1) != 0) goto cleanup;
    }

    // Populate the result.
    out->cookie_count = resp.cookie_count;
    for (size_t i = 0; i < resp.cookie_count; i++) {
        out->cookie_len[i] = resp.cookie_len[i];
        memcpy(out->cookies[i], resp.cookies[i], resp.cookie_len[i]);
    }
    memcpy(out->ntp_host, resp.ntp_host, sizeof out->ntp_host);
    out->ntp_port = resp.ntp_port;
    out->ok = 1;
    rc = 0;

    // Graceful TLS shutdown -- ignore failures; we already have what
    // we came for.
    PinnedTls_CloseNotify(&tls);

cleanup:
    PinnedTls_Free(&tls);
    WSACleanup();
    if (rc != 0) {
        // Wipe any key material we may have written before failing,
        // and drop rotation evidence from a failed exchange.
        mbedtls_platform_zeroize(out, sizeof *out);
        if (rot) memset(rot, 0, sizeof *rot);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Nts_FetchSample -- KE + one authenticated SNTP round trip
// ---------------------------------------------------------------------------

#define NTP_EPOCH_DELTA_S  2208988800ULL
#define NTS_UDP_TIMEOUT_MS 3000
#define NTS_UDP_MAX_PKT    1500

// Server-claimed error from the (AEAD-authenticated) NTP header: root
// dispersion + root delay/2, 16.16 fixed-point seconds at bytes 4-11.
static uint32_t nts_root_err_ms(const uint8_t *pkt) {
    uint32_t beDelay, beDisp;
    memcpy(&beDelay, pkt + 4, 4);
    memcpy(&beDisp,  pkt + 8, 4);
    uint64_t delayMs = ((uint64_t)ntohl(beDelay) * 1000ULL) >> 16;
    uint64_t dispMs  = ((uint64_t)ntohl(beDisp)  * 1000ULL) >> 16;
    uint64_t err = dispMs + delayMs / 2;
    return err > 0x7fffffffULL ? 0x7fffffffU : (uint32_t)err;
}

static int parse_sntp_reply(const uint8_t *pkt, size_t pkt_len,
                            int64_t *out_t2_ms, int64_t *out_t3_ms)
{
    if (pkt_len < 48) return 0;
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (li == 3)                       return 0;
    if (vn != 3 && vn != 4)            return 0;
    if (mode != 4)                     return 0;
    if (stratum == 0 || stratum >= 16) return 0;

    uint32_t secBE, fracBE;
    memcpy(&secBE,  pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s = ntohl(secBE), t2_frac = ntohl(fracBE);
    memcpy(&secBE,  pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s = ntohl(secBE), t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;

    *out_t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    *out_t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    return 1;
}

// ---------------------------------------------------------------------------
// Cookie jar (RFC 8915 cookie reuse)
// ---------------------------------------------------------------------------
//
// An NTS-KE handshake yields up to 8 cookies plus the C2S/S2C keys. RFC
// 8915 intends those cookies to be REUSED: each authenticated NTP
// exchange spends one cookie and the reply supplies a fresh replacement,
// so a full TLS handshake is needed only at first contact, when the jar
// empties, or when the server rejects a cookie. Previously Lunar did a
// full KE every cycle -- ~2,880 handshakes/day/slot against a thin
// 4-family provider pool. The jar fixes that.
//
// Keys and cookies from one KE session belong together (the keys
// authenticate exchanges that spend that session's cookies), so the jar
// holds them as a unit, keyed by provider host. Only cookies from a
// fully ENROLLED KE are jarred: a rotation-pending KE (unverified leaf)
// never populates a reusable jar, so a jar sample is always an enrolled
// pin -- exactly what the aggregator wants.
//
// Guarded by a critical section: two NTS worker threads draw different
// providers within a cycle, but consecutive cycles can overlap.

#define NTS_JAR_SLOTS 12

// Absolute age guard: a jarred cookie set older than this is treated as empty
// (fall straight to a fresh KE) rather than spending a cookie the server has
// likely already expired -- e.g. after the laptop slept for hours. RFC 8915
// cookie lifetimes vary and are not guaranteed multi-hour, so 1 h is a safe
// floor that never wastes a doomed authenticated round trip on wake.
#define NTS_JAR_MAX_AGE_MS  (60ULL * 60ULL * 1000ULL)

typedef struct {
    char     host[NTSKE_MAX_NTP_HOST_LEN + 1];   // jar key = provider host
    int      valid;
    uint8_t  c2s_key[32];
    uint8_t  s2c_key[32];
    char     ntp_host[NTSKE_MAX_NTP_HOST_LEN + 1];
    uint16_t ntp_port;
    size_t   cookie_count;
    size_t   cookie_len[NTSKE_MAX_COOKIES];
    uint8_t  cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    uint64_t last_use_tick;
    uint64_t stored_tick;   // GetTickCount64() when this KE's cookies were jarred
} NtsJar;

static NtsJar           g_jars[NTS_JAR_SLOTS];
static CRITICAL_SECTION g_jar_cs;
static volatile LONG    g_jar_cs_ready = 0;

static void jar_cs_ensure(void) {
    if (InterlockedCompareExchange(&g_jar_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_jar_cs);
        g_jar_cs_ready = 2;
    }
    while (g_jar_cs_ready != 2) { Sleep(0); }
}

// caller holds g_jar_cs
static NtsJar *jar_find(const char *host) {
    for (int i = 0; i < NTS_JAR_SLOTS; i++) {
        if (g_jars[i].valid && strcmp(g_jars[i].host, host) == 0)
            return &g_jars[i];
    }
    return nullptr;
}

// Pop one cookie for `host` into (c2s,s2c,ntp_host,ntp_port,cookie).
// Returns 1 if a jarred cookie was available, 0 otherwise.
static int jar_take(const char *host,
                    uint8_t c2s[32], uint8_t s2c[32],
                    char *ntp_host, uint16_t *ntp_port,
                    uint8_t *cookie, size_t *cookie_len) {
    jar_cs_ensure();
    int got = 0;
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    if (j && j->cookie_count > 0 &&
        (GetTickCount64() - j->stored_tick) > NTS_JAR_MAX_AGE_MS) {
        // Cookies too old to trust the server still honors them; drop the whole
        // set so this exchange does a fresh KE instead of a doomed round trip.
        j->cookie_count = 0;
    }
    if (j && j->cookie_count > 0) {
        memcpy(c2s, j->c2s_key, 32);
        memcpy(s2c, j->s2c_key, 32);
        _snprintf(ntp_host, NTSKE_MAX_NTP_HOST_LEN + 1, "%s", j->ntp_host);
        *ntp_port = j->ntp_port;
        size_t k = --j->cookie_count;    // spend the last cookie
        *cookie_len = j->cookie_len[k];
        memcpy(cookie, j->cookies[k], j->cookie_len[k]);
        j->last_use_tick = GetTickCount64();
        got = 1;
    }
    LeaveCriticalSection(&g_jar_cs);
    return got;
}

// Replace (or create) the jar for `host` with a fresh KE's keys, NTP
// endpoint, and cookie set. Evicts the least-recently-used slot if full.
static void jar_store(const char *host,
                      const uint8_t c2s[32], const uint8_t s2c[32],
                      const char *ntp_host, uint16_t ntp_port,
                      const uint8_t (*cookies)[NTSKE_MAX_COOKIE_LEN],
                      const size_t *lens, size_t count) {
    if (count == 0) return;
    if (count > NTSKE_MAX_COOKIES) count = NTSKE_MAX_COOKIES;
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    if (!j) {
        // Free slot, else LRU eviction.
        NtsJar *lru = &g_jars[0];
        for (int i = 0; i < NTS_JAR_SLOTS; i++) {
            if (!g_jars[i].valid) { lru = &g_jars[i]; break; }
            if (g_jars[i].last_use_tick < lru->last_use_tick) lru = &g_jars[i];
        }
        j = lru;
        mbedtls_platform_zeroize(j, sizeof *j);
    }
    _snprintf(j->host, sizeof j->host, "%s", host);
    memcpy(j->c2s_key, c2s, 32);
    memcpy(j->s2c_key, s2c, 32);
    _snprintf(j->ntp_host, sizeof j->ntp_host, "%s", ntp_host ? ntp_host : "");
    j->ntp_port = ntp_port;
    j->cookie_count = count;
    for (size_t i = 0; i < count; i++) {
        j->cookie_len[i] = lens[i];
        memcpy(j->cookies[i], cookies[i], lens[i]);
    }
    j->stored_tick = GetTickCount64();   // for the absolute cookie-age guard
    j->valid = 1;
    j->last_use_tick = GetTickCount64();
    LeaveCriticalSection(&g_jar_cs);
}

// Append harvested replacement cookies to the existing jar for `host`
// (capped at NTSKE_MAX_COOKIES). No-op if the jar was dropped.
static void jar_add_cookies(const char *host,
                            const uint8_t (*cookies)[NTSKE_MAX_COOKIE_LEN],
                            const size_t *lens, size_t count) {
    if (count == 0) return;
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    if (j) {
        for (size_t i = 0; i < count && j->cookie_count < NTSKE_MAX_COOKIES; i++) {
            size_t k = j->cookie_count++;
            j->cookie_len[k] = lens[i];
            memcpy(j->cookies[k], cookies[i], lens[i]);
        }
        j->last_use_tick = GetTickCount64();
    }
    LeaveCriticalSection(&g_jar_cs);
}

static void jar_drop(const char *host) {
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    if (j) mbedtls_platform_zeroize(j, sizeof *j);
    LeaveCriticalSection(&g_jar_cs);
}

static int jar_cookie_count(const char *host) {
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    int n = j ? (int)j->cookie_count : 0;
    LeaveCriticalSection(&g_jar_cs);
    return n;
}

#ifdef LUNAR_TESTING
void Nts_TestJarReset(void) {
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    mbedtls_platform_zeroize(g_jars, sizeof g_jars);
    LeaveCriticalSection(&g_jar_cs);
}
int Nts_TestJarCount(const char *host) {
    jar_cs_ensure();
    EnterCriticalSection(&g_jar_cs);
    NtsJar *j = jar_find(host);
    int n = j ? (int)j->cookie_count : -1;   // -1 = no jar
    LeaveCriticalSection(&g_jar_cs);
    return n;
}
void Nts_TestJarStore(const char *host, int count) {
    uint8_t keys[32]; memset(keys, 0xAB, sizeof keys);
    uint8_t cks[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  lens[NTSKE_MAX_COOKIES];
    if (count > NTSKE_MAX_COOKIES) count = NTSKE_MAX_COOKIES;
    for (int i = 0; i < count; i++) { memset(cks[i], i + 1, 16); lens[i] = 16; }
    jar_store(host, keys, keys, "ntp.example", 123,
              (const uint8_t (*)[NTSKE_MAX_COOKIE_LEN])cks, lens, (size_t)count);
}
int Nts_TestJarTake(const char *host) {
    uint8_t c2s[32], s2c[32], cookie[NTSKE_MAX_COOKIE_LEN];
    char nh[NTSKE_MAX_NTP_HOST_LEN + 1]; uint16_t np; size_t cl;
    return jar_take(host, c2s, s2c, nh, &np, cookie, &cl);
}
void Nts_TestJarAdd(const char *host, int count) {
    uint8_t cks[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  lens[NTSKE_MAX_COOKIES];
    if (count > NTSKE_MAX_COOKIES) count = NTSKE_MAX_COOKIES;
    for (int i = 0; i < count; i++) { memset(cks[i], 0x55, 16); lens[i] = 16; }
    jar_add_cookies(host, (const uint8_t (*)[NTSKE_MAX_COOKIE_LEN])cks,
                    lens, (size_t)count);
}
void Nts_TestJarDrop(const char *host) { jar_drop(host); }
#endif

// One authenticated SNTP exchange with a given key set and cookie:
// build the request, run the QPC-bracketed UDP round trip, authenticate
// and parse the reply, and report the timing plus any replacement
// cookies the server returned. Returns 1 on success, 0 on any failure.
static int nts_udp_exchange(const char *host, uint16_t port,
                            const uint8_t c2s[32], const uint8_t s2c[32],
                            const uint8_t *cookie, size_t cookie_len,
                            int64_t *out_ntpUtcMs, int64_t *out_qpcAtT4,
                            uint32_t *out_rttMs, uint32_t *out_rootErrMs,
                            uint8_t (*harvest)[NTSKE_MAX_COOKIE_LEN],
                            size_t *harvest_lens, size_t *harvest_cnt) {
    *harvest_cnt = 0;

    uint8_t hdr[48];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0x23;   // LI=0, VN=4, Mode=3 (client)

    uint8_t uid[NTS_UNIQUE_ID_LEN], nonce[NTS_NONCE_LEN];
    uint8_t pkt[NTS_UDP_MAX_PKT];
    size_t  pkt_len = 0;
    int     rc = 0;
    SOCKET  s = INVALID_SOCKET;

    if (BCryptGenRandom(NULL, uid, sizeof uid,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
    if (BCryptGenRandom(NULL, nonce, sizeof nonce,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        mbedtls_platform_zeroize(uid, sizeof uid);
        return 0;
    }
    if (NtsEf_BuildRequest(hdr, uid, nonce, cookie, cookie_len,
                           0 /* no placeholder cookies */, c2s,
                           pkt, sizeof pkt, &pkt_len) != 0) goto done;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) goto done;

    // Dual-stack DoH with NTS-only system-DNS fallback (safe: the reply
    // is AEAD-authenticated, so a forged answer cannot forge a sample).
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) {
        if (Dns_ResolveSystem(host, ip, &fam) != 0) goto net_done;
    }
    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, port, &sa, &salen) == AF_UNSPEC) goto net_done;
    s = Net_DgramSocket(fam, NTS_UDP_TIMEOUT_MS);
    if (s == INVALID_SOCKET) goto net_done;

    int64_t qpcT1 = Clock_Qpc();
    int sent = sendto(s, (const char *)pkt, (int)pkt_len, 0,
                      (struct sockaddr *)&sa, salen);
    if (sent != (int)pkt_len) goto net_done;

    uint8_t reply[NTS_UDP_MAX_PKT];
    int recvd = recv(s, (char *)reply, (int)sizeof reply, 0);
    int64_t qpcT4 = Clock_Qpc();
    if (recvd <= 0) goto net_done;

    if (NtsEf_ParseResponse(reply, (size_t)recvd, uid, s2c,
                            harvest, harvest_lens, harvest_cnt) != 0) {
        *harvest_cnt = 0;
        goto net_done;
    }

    int64_t t2_ms = 0, t3_ms = 0;
    if (!parse_sntp_reply(reply, (size_t)recvd, &t2_ms, &t3_ms)) goto net_done;

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) goto net_done;
    int64_t rtt = ((qpcT4 - qpcT1) * 1000LL + qpcFreq / 2) / qpcFreq;
    if (rtt < 0) rtt = 0;
    int64_t serverProc = t3_ms - t2_ms;
    if (serverProc < 0) serverProc = 0;
    int64_t netRtt = rtt - serverProc;
    if (netRtt < 0) netRtt = 0;

    *out_ntpUtcMs = t3_ms + netRtt / 2;
    *out_qpcAtT4  = qpcT4;
    *out_rttMs    = (uint32_t)(rtt > 0x7fffffff ? 0x7fffffff : rtt);
    if (out_rootErrMs) *out_rootErrMs = nts_root_err_ms(reply);
    rc = 1;

net_done:
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
done:
    mbedtls_platform_zeroize(uid,   sizeof uid);
    mbedtls_platform_zeroize(nonce, sizeof nonce);
    mbedtls_platform_zeroize(pkt,   sizeof pkt);
    return rc;
}

int Nts_FetchSample(const NtsProvider *p,
                    int64_t  *out_ntpUtcMs,
                    int64_t  *out_qpcAtT4,
                    uint32_t *out_rttMs)
{
    return Nts_FetchSampleEx(p, out_ntpUtcMs, out_qpcAtT4, out_rttMs,
                             NULL, NULL);
}

int Nts_FetchSampleEx(const NtsProvider *p,
                      int64_t  *out_ntpUtcMs,
                      int64_t  *out_qpcAtT4,
                      uint32_t *out_rttMs,
                      uint32_t *out_rootErrMs,
                      NtsRotationPending *rot)
{
    if (rot) memset(rot, 0, sizeof *rot);
    if (p == NULL || out_ntpUtcMs == NULL || out_qpcAtT4 == NULL
        || out_rttMs == NULL) return 0;
    *out_ntpUtcMs = 0; *out_qpcAtT4 = 0; *out_rttMs = 0;
    if (out_rootErrMs) *out_rootErrMs = 0;

    uint8_t harvest[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  harvest_lens[NTSKE_MAX_COOKIES];
    size_t  harvest_cnt = 0;

    // --- Path A: reuse a jarred cookie, no handshake -----------------------
    // A jar only ever holds cookies from a fully enrolled KE, so a
    // reused sample is inherently an enrolled pin -- rot stays zeroed.
    {
        uint8_t c2s[32], s2c[32], cookie[NTSKE_MAX_COOKIE_LEN];
        char    nhost[NTSKE_MAX_NTP_HOST_LEN + 1];
        uint16_t nport = 0;
        size_t  clen = 0;
        if (jar_take(p->host, c2s, s2c, nhost, &nport, cookie, &clen)) {
            const char *host = nhost[0] ? nhost : p->host;
            uint16_t    port = nport ? nport : 123;
            int r = nts_udp_exchange(host, port, c2s, s2c, cookie, clen,
                                     out_ntpUtcMs, out_qpcAtT4, out_rttMs,
                                     out_rootErrMs,
                                     harvest, harvest_lens, &harvest_cnt);
            mbedtls_platform_zeroize(c2s, sizeof c2s);
            mbedtls_platform_zeroize(s2c, sizeof s2c);
            mbedtls_platform_zeroize(cookie, sizeof cookie);
            if (r == 1) {
                jar_add_cookies(p->host, (const uint8_t (*)[NTSKE_MAX_COOKIE_LEN])harvest,
                                harvest_lens, harvest_cnt);
                // Steady-state confirmation: log when the jar level changes,
                // or hourly, never on every reuse.
                {
                    static volatile LONG s_lastJar[NTS_PROVIDER_COUNT];
                    static volatile LONG64 s_lastJarLog[NTS_PROVIDER_COUNT];
                    size_t idx = provider_index(p);
                    int jar = jar_cookie_count(p->host);
                    LONG64 now = (LONG64)GetTickCount64();
                    int emit = 1;
                    if (idx < NTS_PROVIDER_COUNT) {
                        emit = s_lastJar[idx] != (LONG)jar || s_lastJarLog[idx] == 0 ||
                               now - s_lastJarLog[idx] >= 60LL * 60LL * 1000LL;
                        if (emit) {
                            InterlockedExchange(&s_lastJar[idx], (LONG)jar);
                            InterlockedExchange64(&s_lastJarLog[idx], now ? now : 1);
                        }
                    }
                    if (emit) {
                        Log_Append("nts: %s cookie reuse ok (jar=%d)", p->label, jar);
                    }
                }
                mbedtls_platform_zeroize(harvest, sizeof harvest);
                return 1;
            }
            // The jarred cookie was rejected or the exchange failed:
            // discard the whole jar for this host and fall through to a
            // fresh KE (its keys are stale once cookies stop working).
            jar_drop(p->host);
            Log_Append("nts: %s jarred cookie rejected; full KE", p->label);
        }
    }

    // --- Path B: full NTS-KE handshake -------------------------------------
    NtsKeResult ke;
    if (Nts_DoKeEx(p, &ke, rot) != 0 || !ke.ok || ke.cookie_count == 0) {
        if (rot) memset(rot, 0, sizeof *rot);
        mbedtls_platform_zeroize(&ke, sizeof ke);
        return 0;
    }

    // Only a fully enrolled KE (matched an existing pin, or a first-run /
    // scheduled CA enrollment) may seed a reusable jar. A rotation-
    // pending KE is unverified until the aggregator corroborates it, so
    // its cookies are used once and never jarred.
    int enrolled = !(rot && rot->pending);
    if (enrolled && ke.cookie_count > 1) {
        jar_store(p->host, ke.c2s_key, ke.s2c_key, ke.ntp_host, ke.ntp_port,
                  (const uint8_t (*)[NTSKE_MAX_COOKIE_LEN])&ke.cookies[1],
                  &ke.cookie_len[1], ke.cookie_count - 1);
    } else {
        jar_drop(p->host);
    }

    const char *host = ke.ntp_host[0] ? ke.ntp_host : p->host;
    uint16_t    port = ke.ntp_port    ? ke.ntp_port : 123;
    harvest_cnt = 0;
    int rc = nts_udp_exchange(host, port, ke.c2s_key, ke.s2c_key,
                              ke.cookies[0], ke.cookie_len[0],
                              out_ntpUtcMs, out_qpcAtT4, out_rttMs,
                              out_rootErrMs,
                              harvest, harvest_lens, &harvest_cnt);
    if (rc == 1 && enrolled) {
        jar_add_cookies(p->host, (const uint8_t (*)[NTSKE_MAX_COOKIE_LEN])harvest,
                        harvest_lens, harvest_cnt);
    }
    Log_Append("nts: %s full KE %s (%u cookies)", p->label,
               rc ? "ok" : "exchange failed", (unsigned)ke.cookie_count);

    mbedtls_platform_zeroize(harvest, sizeof harvest);
    mbedtls_platform_zeroize(&ke, sizeof ke);
    if (rc == 0 && rot) memset(rot, 0, sizeof *rot);
    return rc;
}
