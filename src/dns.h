// dns.h -- DNS-over-HTTPS resolver with local SPKI enrollment and
// randomized enrolled-resolver failover. The ONLY DNS path this binary uses.
//
// =============================================================================
// Why this module exists
// =============================================================================
//
// Every upstream socket (SNTP to core, TLS to NTS-KE, UDP to the NTS
// NTP server) starts with a hostname -> IP resolution. Plain DNS over
// UDP/53 is trivially spoofable by any on-path attacker and by any
// compromised forwarder the OS happens to be configured with. That
// defeats the entire trust chain: an attacker who controls the DNS
// reply for time.cloudflare.com can redirect our NTS-KE session to
// their own TLS server. The SPKI pin would still catch it (they don't
// have the right leaf key), but the pin only guards the cases where we
// actually reached the pinned operator; a forged DNS reply for a core
// SNTP host simply redirects SNTP to a fake UDP listener.
//
// =============================================================================
// Design
// =============================================================================
//
// 1. All resolution goes through DNS-over-HTTPS (RFC 8484): one POST per
//    connection over HTTP/2 (h2.c, offered first via ALPN) or HTTP/1.1.
//    Quad9 and Mullvad serve DoH over HTTP/2 only. Bootstrap is from a
//    hardcoded table of 5 well-known resolvers, each identified by (a) a
//    hardcoded anycast IPv4 address so we never need DNS to find the DNS,
//    and (b) a pinned SHA-256(SPKI) of the TLS leaf certificate so a
//    compromised CA cannot mint a trusted substitute.
//
//       cloudflare    1.1.1.1, 1.0.0.1
//       quad9         9.9.9.9, 149.112.112.112
//       google        8.8.8.8, 8.8.4.4
//       nextdns       45.90.28.0
//       mullvad       194.242.2.2
//
// 2. Per cache-miss we shuffle the enabled resolver pool
//    (Fisher-Yates over the table, BCryptGenRandom) and try each
//    pinned resolver in that random order until one succeeds. For
//    each resolver, primary anycast IP is tried first; on TCP/TLS
//    failure we fall back to the resolver's secondary IP if one is
//    listed. A successful reply's A records are cached under a TTL
//    clamped to [60s, 3600s]; the first A record becomes the returned
//    IP.
//
//    An earlier design queried two resolvers and required their
//    A-record sets to overlap. That failed constantly against
//    legitimate geo-aware authoritative DNS: Cloudflare's resolver
//    sees the query from Cloudflare's network and gets back IPs near
//    that edge; Google's resolver sees it from Google's network and
//    gets back a disjoint set. Both answers are honest and correct;
//    the sets just don't overlap. We removed that resolver-level
//    overlap check because the single-operator-compromise threat it
//    was meant to catch is already neutralised downstream (see Threat
//    model below).
//
// 3. Hard fail. There is NO plain-DNS fallback. A network that blocks
//    all pinned resolvers simultaneously will push Lunar to INOP,
//    which is the correct outcome -- we MUST NOT silently degrade to
//    spoofable UDP/53 just because the secure path is unavailable.
//    An attacker who could force such a fallback would have defeated
//    the whole purpose of this module.
//
// 4. In-memory cache, 32 entries, per-host TTL = min(DNS TTL, 1 hour)
//    with a 60-second floor. Cleared on process restart (bootstrap
//    IPs are hardcoded, so the cache is never load-bearing for
//    reachability). The cache is protected by its own critical
//    section; concurrent worker threads may call Dns_Resolve freely.
//
// 5. Dual-stack. Each resolver carries IPv6 anycast bootstrap addresses
//    alongside its IPv4 ones, so DoH is reachable on an IPv6-only
//    network. Resolution queries both A and AAAA; the address family
//    that most recently reached a DoH resolver is preferred (a working
//    signal for the local network), with the other family as fallback.
//    Every upstream socket path (SNTP, NTS-KE, NTS-NTP) connects via the
//    family-agnostic helpers in netutil.h.
//
// =============================================================================
// Threat model
// =============================================================================
//
// - On-path attacker with UDP/53 manipulation: defeated (we never
//   issue a UDP/53 query).
// - Compromised OS resolver config / hosts file: defeated (we bypass
//   getaddrinfo entirely).
// - Compromised CA minting a cert for cloudflare-dns.com: defeated by
//   SPKI pin on the DoH leaf.
// - Compromise of ONE DoH resolver operator (returns a forged A set
//   for, say, time.cloudflare.com): caught DOWNSTREAM, not here.
//     * NTS-KE: TLS to the time operator is authenticated by a local
//       enrolled SPKI pin, so a DNS-level redirect fails at TLS.
//     * NTS SNTP: packets are AEAD-authenticated with c2s/s2c keys
//       derived from that pinned KE session, so a lying IP cannot
//       forge a valid sample.
//     * Core SNTP: offsets must agree within 200 ms with the midpoint
//       of the two authenticated NTS samples, so a lying core IP
//       cannot bias the clock.
//   Randomized first choice over 5 operators additionally limits a
//   compromised operator to ~1/5 of successful first answers on
//   average; failover only helps transport/TLS/query failures.
// - Network blocking port 443 to all pinned resolvers: not a security
//   failure, just a denial-of-service. We go INOP and display that.
//
// =============================================================================

#ifndef LUNAR_DNS_H
#define LUNAR_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Public API --------------------------------------------------------------

// Resolve `host` to an IPv4 dotted-quad string in `out_ip` (buffer
// must be at least 16 bytes). Legacy IPv4-only wrapper over
// Dns_ResolveEx: succeeds only if an A record is available. Returns 0
// on success, -1 otherwise.
int Dns_Resolve(const char *host, char out_ip[16]);

// Resolve `host` via pinned DoH to an IPv4 or IPv6 literal in `out_ip`
// (buffer must be at least NET_IP_STRLEN bytes, see netutil.h), writing
// the address family (AF_INET / AF_INET6) to *out_family. Queries both
// A and AAAA, preferring the family that most recently reached a DoH
// resolver. Returns 0 on success, -1 on failure (no resolver reachable,
// no records, or a bad host).
//
// Thread-safe. May block for several seconds on a cache miss.
int Dns_ResolveEx(const char *host, char *out_ip, int *out_family);

// System-DNS (getaddrinfo) resolution, dual-stack. This is the ONLY
// path that bypasses pinned DoH, and it is for NTS hostnames ONLY: an
// NTS-KE session is authenticated by a locally enrolled SPKI pin, so a
// forged system-DNS answer cannot redirect it to an attacker's server
// (the pin catches it at TLS). It MUST NOT be used for unauthenticated
// core SNTP, where a forged answer would redirect to a fake listener.
// Writes a literal + family like Dns_ResolveEx. Returns 0 / -1.
int Dns_ResolveSystem(const char *host, char *out_ip, int *out_family);

// Clear the entire in-memory cache. Primarily for tests.
void Dns_CacheClear(void);

// Drop any cached entry for `host` (e.g. after a connect to the cached IP
// failed) so the next resolve does a fresh lookup instead of reusing a
// possibly rotated/dead address for the full TTL floor.
void Dns_Invalidate(const char *host);

// --- Pool introspection (for tests + random picks) --------------------------

typedef struct {
    const char *hostname;          // SNI + HTTP Host:, e.g. "cloudflare-dns.com"
    const char *ip_primary;         // hardcoded anycast IPv4 dotted-quad
    const char *ip_secondary;       // NULL if no secondary
    const char *ip6_primary;        // hardcoded anycast IPv6 literal, or NULL
    const char *ip6_secondary;      // NULL if no secondary
    const char *label;              // short tag for logs, e.g. "cloudflare"
    const char *operator_family;    // independent operator grouping
} DnsResolver;

// Return the full resolver metadata table and its length.
const DnsResolver *Dns_Pool(size_t *out_len);

// Fisher-Yates partial shuffle over the resolver metadata pool.
// Writes up to `n_want` distinct resolver pointers to out[0..n-1].
// Returns the number actually written (<= n_want, <= pool size). 0 on
// bad args or RNG failure.
size_t Dns_PickResolvers(const DnsResolver **out, size_t n_want);

// --- DNS wire format (exported for tests) -----------------------------------
//
// Minimal RFC 1035 encoder/decoder, A records only, no compression on
// the wire we emit (servers may use compression in responses; we
// follow pointers during parse). No EDNS -- DoH already gives us the
// secure channel, and EDNS adds parsing surface for no gain.

// Build a DNS query for host/A. Writes `*out_len` bytes on success.
// Caller supplies `id` (typically from a CSPRNG). Returns 0 on
// success, -1 if out is too small or the host is syntactically
// invalid (labels > 63 octets, total > 255 octets, empty labels).
int Dns_BuildQueryA(const char *host, uint16_t id,
                    uint8_t *out, size_t out_cap, size_t *out_len);

// Parse a DNS response for A records. Writes up to `ips_cap`
// dotted-quad strings (each 16 bytes) into `out_ips`, and writes the
// number actually written to `*out_count`. Also writes the smallest
// TTL among the A records to `*out_min_ttl` (clamped sensibly by the
// caller). Returns 0 on success, -1 on malformed response, -2 on RCODE
// != NOERROR, -3 if QID/question mismatch the expected `expect_id`
// and `expect_host`. CNAME chains are followed to the terminal A
// records; other RR types are skipped.
int Dns_ParseResponseA(const uint8_t *in, size_t in_len,
                       uint16_t expect_id, const char *expect_host,
                       char (*out_ips)[16], size_t ips_cap,
                       size_t *out_count,
                       uint32_t *out_min_ttl);

// General A/AAAA response parser. want_qtype is 1 (A) or 28 (AAAA); the
// address strings land in NET_IP_STRLEN-wide buffers (see netutil.h).
// Same return codes as Dns_ParseResponseA. Exported for tests.
int Dns_ParseResponse(const uint8_t *in, size_t in_len,
                      uint16_t expect_id, const char *expect_host,
                      uint16_t want_qtype,
                      char (*out_ips)[46], size_t ips_cap,
                      size_t *out_count, uint32_t *out_min_ttl);

// --- A-record set helper (exported for tests) -------------------------------

// Given two A-record sets, find the first IP from `set_a` that also
// appears in `set_b`. Writes it to out_ip (16-byte buffer) and
// returns 0. Returns -1 if no overlap. Case-sensitive string compare
// on dotted quads.
int Dns_Intersect(const char (*set_a)[16], size_t n_a,
                  const char (*set_b)[16], size_t n_b,
                  char out_ip[16]);

#ifdef __cplusplus
}
#endif

#endif
