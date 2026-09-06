// ntp.c -- Parallel SNTP+NTS client. Every polling cycle, six sources
// are queried in parallel:
//
//    slots 0..3: four core (plain-SNTP) sources, randomly drawn
//                per-cycle from the kCorePool[] list of reputable
//                stratum-1 national metrology / research time servers
//                across diverse continents, operators, and upstream
//                routes. Leap-smearing operators (Google, Facebook,
//                AWS) are DELIBERATELY EXCLUDED because their 12-24h
//                smear window produces false disagreement with the
//                non-smearing metrology institutes during that
//                window, which would poison the concurrence gate.
//
//    slots 4..5: two NTS-authenticated SNTP sources, randomly drawn
//                per-cycle from the NTS provider metadata pool in
//                src/nts.c. TLS 1.3 + ALPN "ntske/1" + local enrolled
//                SPKI pin / Windows CA renewal; picks prefer distinct
//                operator families within a cycle.
//
// One UDP socket per core source + one TCP+UDP pair per NTS slot,
// one worker thread per source, all fired in parallel. Results are
// collected per-source and a single concurrence verdict is computed:
//
//   * both NTS slots must succeed, be authenticated by enrolled pins,
//     come from different operator families, and agree within 200 ms
//     of each other (projected to a common QPC moment). The anchor is
//     then the midpoint of the two NTS samples.
//
//   * at least 3 of the 4 core sources must agree with the NTS
//     midpoint to within 200 ms. The 3-of-4 super-majority tolerates
//     one national outage or hijack while still rejecting a
//     coordinated attack against a minority of core sources.
//
//   * otherwise (both NTS failed, NTS disagreement, too few cores
//     concurring), the clockwork goes INOP.
//
// This translation unit is isolated from D2D headers because
// <winsock2.h> + <windows.h> collide with their symbol names.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "ntp.h"
#include "clock.h"
#include "nts.h"
#include "dns.h"
#include "netutil.h"
#include "logbuf.h"
#include "app_paths.h"
#include "pin_store.h"

#include <bcrypt.h>

#define NTP_PORT             "123"
#define NTP_PORT_NUM         123
#define NTP_TIMEOUT_MS       6000      // core slot UDP recv timeout
#define NTS_SLOT_TIMEOUT_MS  20000     // KE (TLS 1.3 + handshake) + authenticated UDP
#define NTP_CORE_SLOT_ATTEMPTS 2       // initial pick + one immediate replacement
#define NTP_NTS_SLOT_ATTEMPTS  2       // initial pick + one immediate replacement
#define NTP_PACKET_LEN       48
#define NTP_EPOCH_DELTA_S    2208988800ULL        // seconds between 1900 and 1970
#define CONCUR_THRESHOLD_MS  200
#define CORE_CLUSTER_THRESHOLD_MS 100  // tight core cluster: widen-only witness
#define NTP_DISP_FLOOR_MS     15  // floor on the server-claimed root error
                                  // (covers t2/t3 quantization + a stingy
                                  // or zeroed root-dispersion field)
#define NTP_CYCLE_TIMEOUT_MS (NTS_SLOT_TIMEOUT_MS * NTP_NTS_SLOT_ATTEMPTS)
#define NTP_WAIT_SLICE_MS    250
#define NTP_SHUTDOWN_WORKER_GRACE_MS      2000
#define NTP_SHUTDOWN_AGGREGATOR_WAIT_MS   5000
#define NTP_SHUTDOWN_DETACHED_WAIT_MS     5000

static_assert(NTP_CORE_COUNT + NTP_NTS_COUNT == NTP_SOURCE_COUNT,
              "NTP source slots must sum to the published total");
static_assert(NTP_FIRST_NTS_SLOT == NTP_CORE_COUNT,
              "NTS slots must immediately follow core slots");
static_assert(NTP_PACKET_LEN == 48,
              "SNTP client packet header must be 48 bytes");

// --- Core pool (plain SNTP) ---------------------------------------------

// Curated pool of reputable stratum-1 public NTP servers, run by
// national metrology institutes, research labs and naval observatories
// around the world. Four random picks are made per cycle (see
// PickCoreSources below) so that any correlated attack or outage only
// affects a subset of cycles.
//
// Deliberately EXCLUDED: Google, Facebook, AWS, Microsoft and other
// "leap-smearing" operators. During a leap-second smear window
// (typically 12-24h) their reported UTC differs from non-smearing
// metrology institutes by up to 1 s; cross-checking them against a
// national lab would falsely trip the concurrence gate.
typedef struct {
    const char *host;
    const char *label;   // short display / log label
    const char *geo;     // short region hint (display only, unused by
                         // verdict logic)
} NtpSource;

static const NtpSource kCorePool[] = {
    { .host = "time.nist.gov",      .label = "NIST",    .geo = "US" }, // Boulder / Gaithersburg
    { .host = "tick.usno.navy.mil", .label = "USNO-a",  .geo = "US" }, // US Naval Observatory (east)
    { .host = "tock.usno.navy.mil", .label = "USNO-b",  .geo = "US" }, // US Naval Observatory (alt)
    { .host = "ptbtime1.ptb.de",    .label = "PTB-1",   .geo = "DE" }, // Physikalisch-Tech. Bundesanstalt
    { .host = "ptbtime2.ptb.de",    .label = "PTB-2",   .geo = "DE" },
    { .host = "ntp1.npl.co.uk",     .label = "NPL-1",   .geo = "UK" }, // Nat. Physical Laboratory
    { .host = "ntp2.npl.co.uk",     .label = "NPL-2",   .geo = "UK" },
    { .host = "ntp.nict.jp",        .label = "NICT",    .geo = "JP" }, // Nat. Inst. of Info. and Comm. Tech.
    { .host = "ntp1.inrim.it",      .label = "INRIM-1", .geo = "IT" }, // Ist. Naz. di Ricerca Metrologica
    { .host = "ntp2.inrim.it",      .label = "INRIM-2", .geo = "IT" },
    { .host = "hora.roa.es",        .label = "ROA",     .geo = "ES" }, // Real Instituto y Observatorio de la Armada
    { .host = "ntp11.metas.ch",     .label = "METAS",   .geo = "CH" }, // Swiss Federal Institute of Metrology
    { .host = "ntp1.sp.se",         .label = "RISE",    .geo = "SE" }, // RISE Research Institutes of Sweden
};

#define CORE_POOL_SIZE   (sizeof kCorePool / sizeof kCorePool[0])

static_assert(CORE_POOL_SIZE >= NTP_CORE_COUNT,
              "core NTP pool must have enough sources for each cycle");
static_assert(NTP_CORE_COUNT * NTP_CORE_SLOT_ATTEMPTS <= CORE_POOL_SIZE,
              "core NTP pool must have replacement sources for each slot");

// Kiss-o'-Death handling (RFC 5905 section 7.4): a stratum-0 reply whose
// reference ID carries an ASCII code like RATE / DENY / RSTR is an
// explicit server request to back off. Honoring it is basic etiquette
// toward the national metrology institutes this pool depends on -- and
// ignoring it invites the IP-level rate limiting that would prolong the
// very outage we are polling to end. Per pool entry, a GetTickCount64
// deadline before which the host is excluded from the cycle draw:
// DENY/RSTR exclude for the rest of the session, everything else for
// KOD_RATE_COOLDOWN_MS. Interlocked access; no lock needed.
#define KOD_RATE_COOLDOWN_MS (15 * 60 * 1000)
static volatile LONG64 g_coreKodUntilTick[CORE_POOL_SIZE];

static void Ntp_MarkKissOfDeath(const NtpSource *src, const char *kiss) {
    size_t i = (size_t)(src - kCorePool);
    if (i >= CORE_POOL_SIZE) return;
    int forever = (memcmp(kiss, "DENY", 4) == 0 ||
                   memcmp(kiss, "RSTR", 4) == 0);
    int64_t until = forever
        ? INT64_MAX
        : (int64_t)GetTickCount64() + KOD_RATE_COOLDOWN_MS;
    InterlockedExchange64(&g_coreKodUntilTick[i], until);
    Log_Append("ntp: %s sent kiss-o'-death %.4s; %s",
               src->label ? src->label : src->host, kiss,
               forever ? "excluded for the rest of the session"
                       : "cooling down for 15 min");
}

// Published label buffers for each NTS slot. Pointed to by
// g_results[4..5].label, so they MUST outlive the aggregator thread
// that filled them -- which is exactly what file-scope storage gives
// us. Guarded by g_cs on write and read.
static char g_ntsLabelPublished[NTP_NTS_COUNT][32] = { { 0 } };

// Placeholder label used for an NTS slot when no provider is available.
static const char kNtsNoPinLabel[] = "NTS--";

// --- Shared state --------------------------------------------------------

static CRITICAL_SECTION g_cs;
static int              g_csInit = 0;

// Results from the most recent polling cycle. Written by the aggregator
// thread under g_cs; read by Ntp_GetResults().
static NtpSourceResult  g_results[NTP_SOURCE_COUNT];
static volatile LONG64  g_offsetMs        = 0;   // legacy accessor
static volatile LONG64  g_lastSuccessTick = 0;   // GetTickCount64() at any-ok
static volatile LONG64  g_lastSuccessUtc  = 0;   // UTC ms at any-ok
static volatile LONG64  g_lastSpreadMs    = 0;   // last cycle's core spread
static volatile LONG64  g_lastNtsSpreadMs = 0;   // last cycle's NTS-pair spread
static volatile LONG    g_running         = 0;
static volatile LONG    g_shutdownRequested = 0;
static volatile LONG    g_activeWorkers     = 0;

static INIT_ONCE        g_lifecycleOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lifecycleCs;
static HANDLE           g_workersIdleEvent = NULL;
static HANDLE           g_aggregatorThread = NULL;

static BOOL CALLBACK Ntp_LifecycleInit(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lifecycleCs);
    g_workersIdleEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
    return g_workersIdleEvent != NULL;
}

static int Ntp_EnsureLifecycle(void)
{
    return InitOnceExecuteOnce(&g_lifecycleOnce, Ntp_LifecycleInit, NULL, NULL)
           && g_workersIdleEvent != NULL;
}

static void Ntp_CloseFinishedAggregatorLocked(void)
{
    if (g_aggregatorThread &&
        WaitForSingleObject(g_aggregatorThread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_aggregatorThread);
        g_aggregatorThread = NULL;
    }
}

static void Ntp_WorkerStarted(void)
{
    InterlockedIncrement(&g_activeWorkers);
    if (g_workersIdleEvent) ResetEvent(g_workersIdleEvent);
}

static void Ntp_WorkerFinished(void)
{
    if (InterlockedDecrement(&g_activeWorkers) == 0 && g_workersIdleEvent) {
        SetEvent(g_workersIdleEvent);
    }
}

// --- Single-source query -------------------------------------------------

// Run one SNTP exchange against a specific host. Writes the result fields
// on success; returns 1 on success, 0 on any error (DNS, socket, timeout,
// invalid header, zero timestamps).
//
// QPC-only timing: we do NOT read the Windows system clock. T1 and T4
// are taken from QueryPerformanceCounter; only the server's reply
// timestamps (T2, T3 in NTP epoch) supply real UTC. The result's
// ntpUtcMs is "the real UTC at the moment qpcAtT4 was sampled":
//
//   rtt_ms         = (qpcT4 - qpcT1) * 1000 / qpcFreq
//   server_proc_ms = t3_ms - t2_ms
//   net_rtt_ms     = max(0, rtt_ms - server_proc_ms)
//   half_net_ms    = net_rtt_ms / 2
//   ntpUtcMs       = t3_ms + half_net_ms    // UTC at our local qpcT4
//
// outOffsetMs is kept for API compatibility but repurposed: it now
// carries ntpUtcMs (absolute UTC at qpcAtT4). The aggregator projects
// those values onto a common QPC moment before computing concurrence.
// outKiss (optional, char[5]): filled with the ASCII kiss-o'-death code
// when the server replies with stratum 0; left untouched otherwise.
// Server-claimed error from the NTP header: root dispersion + root
// delay/2, both 16.16 fixed-point seconds at fixed offsets (bytes 4-11).
// Under NTS the header is AEAD-authenticated, so the claim is the
// server's own, not an on-path forger's.
static uint32_t NtpRootErrMs(const unsigned char *pkt) {
    uint32_t beDelay, beDisp;
    memcpy(&beDelay, pkt + 4, 4);
    memcpy(&beDisp,  pkt + 8, 4);
    uint64_t delayMs = ((uint64_t)ntohl(beDelay) * 1000ULL) >> 16;
    uint64_t dispMs  = ((uint64_t)ntohl(beDisp)  * 1000ULL) >> 16;
    uint64_t err = dispMs + delayMs / 2;
    return err > 0x7fffffffULL ? 0x7fffffffU : (uint32_t)err;
}

static int NtpQueryHost(const char *host,
                        int64_t *outOffsetMs,
                        int64_t *outNtpUtcMs,
                        int64_t *outQpcAtT4,
                        uint32_t *outRttMs,
                        uint32_t *outRootErrMs,
                        char *outKiss) {
    unsigned char pkt[NTP_PACKET_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;  // LI=0, VN=4, Mode=3 (client)

    // Resolve via pinned DoH with randomized resolver failover so an
    // on-path attacker cannot redirect this SNTP packet to a fake UDP
    // listener by forging a plain-DNS reply. See src/dns.h for the
    // design rationale. Dual-stack (A or AAAA), but NO fallback to
    // getaddrinfo: core SNTP is unauthenticated, so system DNS would be
    // a spoofing hole. If all pinned DoH resolvers fail, this source
    // fails and the cycle degrades.
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) return 0;

    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, NTP_PORT_NUM, &sa, &salen) == AF_UNSPEC) return 0;

    SOCKET s = Net_DgramSocket(fam, NTP_TIMEOUT_MS);
    if (s == INVALID_SOCKET) return 0;

    int64_t qpcT1 = Clock_Qpc();
    int sent = sendto(s, (const char *)pkt, (int)sizeof(pkt), 0,
                      (struct sockaddr *)&sa, salen);
    if (sent != (int)sizeof(pkt)) { closesocket(s); return 0; }

    int recvd = recv(s, (char *)pkt, (int)sizeof(pkt), 0);
    int64_t qpcT4 = Clock_Qpc();
    closesocket(s);
    if (recvd != (int)sizeof(pkt)) return 0;

    // Validate the response header before trusting any timestamps:
    //   LI  (bits 7..6 of byte 0) must be 0, 1 or 2 (3 = alarm, unsynced)
    //   VN  (bits 5..3) must be 3 or 4
    //   Mode(bits 2..0) must be 4                   (server)
    //   Stratum (byte 1) must be 1..15              (0 = kiss-o'-death,
    //                                                16 = unsynchronized)
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (vn != 3 && vn != 4)              return 0;
    if (mode != 4)                       return 0;
    if (stratum == 0) {
        // Kiss-o'-Death: the reference ID (bytes 12..15) carries an
        // ASCII back-off code (RFC 5905 section 7.4). Surface it so the
        // worker can honor the server's request.
        if (outKiss) { memcpy(outKiss, pkt + 12, 4); outKiss[4] = 0; }
        return 0;
    }
    if (li == 3)                         return 0;
    if (stratum >= 16)                   return 0;

    uint32_t secBE, fracBE;
    memcpy(&secBE, pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s    = ntohl(secBE);
    uint32_t t2_frac = ntohl(fracBE);
    memcpy(&secBE, pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s    = ntohl(secBE);
    uint32_t t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;

    int64_t t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    int64_t t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) return 0;
    int64_t rtt = ((qpcT4 - qpcT1) * 1000LL + qpcFreq / 2) / qpcFreq;
    if (rtt < 0) rtt = 0;

    int64_t serverProc = t3_ms - t2_ms;
    if (serverProc < 0) serverProc = 0;
    int64_t netRtt = rtt - serverProc;
    if (netRtt < 0) netRtt = 0;

    *outNtpUtcMs = t3_ms + netRtt / 2;
    *outQpcAtT4  = qpcT4;
    *outRttMs    = (uint32_t)(rtt > 0x7fffffff ? 0x7fffffff : rtt);
    if (outRootErrMs) *outRootErrMs = NtpRootErrMs(pkt);
    // offsetMs carries absolute UTC at qpcAtT4 (legacy field reused).
    *outOffsetMs = *outNtpUtcMs;
    return 1;
}

// --- Per-source worker thread --------------------------------------------

typedef struct {
    volatile LONG    refs;
    const NtpSource *candidates[NTP_CORE_SLOT_ATTEMPTS];
    size_t           candidate_count;
    const NtpSource *src;      // successful source, or last attempted source
    int              slot;
    int              attempts;
    NtpSourceResult  out;      // written by worker, read by aggregator
} WorkerCtx;

static void WorkerCtx_AddRef(WorkerCtx *ctx) {
    if (ctx) InterlockedIncrement(&ctx->refs);
}

static void WorkerCtx_Release(WorkerCtx *ctx) {
    if (ctx && InterlockedDecrement(&ctx->refs) == 0) free(ctx);
}

// Core (unauthenticated SNTP) worker. Used for slots 0..NTP_CORE_COUNT-1.
static DWORD WINAPI WorkerProc(LPVOID param) {
    WorkerCtx *ctx = (WorkerCtx *)param;
    int wsa_started = 0;

    NtpSourceResult *r = &ctx->out;
    memset(r, 0, sizeof(*r));
    r->label = "?";

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        r->ok = 0;
        goto done;
    }
    wsa_started = 1;

    for (size_t i = 0; i < ctx->candidate_count; i++) {
        const NtpSource *src = ctx->candidates[i];
        if (!src) continue;

        ctx->src = src;
        ctx->attempts = (int)i + 1;
        r->label = src->label;

        int64_t off = 0, utc = 0, qpc = 0;
        uint32_t rtt = 0, rootErr = 0;
        char kiss[5] = { 0 };
        if (NtpQueryHost(src->host, &off, &utc, &qpc, &rtt, &rootErr, kiss)) {
            r->ok        = 1;
            r->offsetMs  = off;
            r->ntpUtcMs  = utc;
            r->qpcAtT4   = qpc;
            r->rttMs     = rtt;
            r->rootErrMs = rootErr;
            r->authMode  = NTP_AUTH_PLAIN_SNTP;
            goto done;
        }
        if (kiss[0]) Ntp_MarkKissOfDeath(src, kiss);

        if (i + 1 < ctx->candidate_count && ctx->candidates[i + 1]) {
            const NtpSource *next = ctx->candidates[i + 1];
            Log_Append("ntp: core slot %d %s failed; retrying immediately with %s",
                       ctx->slot,
                       src->label ? src->label : "?",
                       next->label ? next->label : "?");
        }
    }

    r->ok = 0;
done:
    if (wsa_started) WSACleanup();
    Ntp_WorkerFinished();
    WorkerCtx_Release(ctx);
    return 0;
}

// Pick `n_want` DISTINCT core pool entries at random (uniform over
// the pool) for one cycle. Fisher-Yates partial shuffle seeded by
// BCryptGenRandom for crypto-grade source-set unpredictability --
// avoiding the "always the same four targets" fingerprint. Returns
// the number actually written (== min(n_want, CORE_POOL_SIZE)).
static size_t PickCoreSources(const NtpSource **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;

    // Exclude hosts honoring a kiss-o'-death cooldown. If that empties
    // the pool entirely, the cycle simply fails -- the servers asked us
    // to go away, and holdover display keeps running regardless.
    size_t idx[CORE_POOL_SIZE];
    size_t pool = 0;
    int64_t now = (int64_t)GetTickCount64();
    for (size_t i = 0; i < CORE_POOL_SIZE; i++) {
        int64_t until = InterlockedCompareExchange64(
            &g_coreKodUntilTick[i], 0, 0);
        if (until > now) continue;
        idx[pool++] = i;
    }
    if (pool == 0) return 0;
    if (n_want > pool) n_want = pool;

    for (size_t i = 0; i < n_want; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            // Crypto RNG failed; degrade to a deterministic linear
            // pick so the cycle still runs. Safety > entropy here.
            for (size_t k = i; k < n_want; k++) out[k] = &kCorePool[idx[k]];
            return n_want;
        }
        size_t j = i + (size_t)(r % (uint32_t)(pool - i));
        size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
        out[i] = &kCorePool[idx[i]];
    }
    return n_want;
}

#ifdef LUNAR_TESTING
void Ntp_TestMarkKissOfDeath(int poolIdx, const char *kiss) {
    if (poolIdx < 0 || (size_t)poolIdx >= CORE_POOL_SIZE) return;
    Ntp_MarkKissOfDeath(&kCorePool[poolIdx], kiss);
}
int Ntp_TestEligibleCoreCount(void) {
    const NtpSource *tmp[CORE_POOL_SIZE];
    return (int)PickCoreSources(tmp, CORE_POOL_SIZE);
}
void Ntp_TestClearKissOfDeath(void) {
    for (size_t i = 0; i < CORE_POOL_SIZE; i++)
        InterlockedExchange64(&g_coreKodUntilTick[i], 0);
}
#endif

// NTS (authenticated) worker. The aggregator pre-selects a distinct
// provider for each slot (see AggregatorProc) so the two NTS picks
// don't collide within one cycle. We perform a full KE + one
// authenticated SNTP round trip. Because the pool is pinned per
// leaf SPKI, an adversary would need to simultaneously defeat TWO
// independent NTS operators' TLS to shift the consensus anchor.
typedef struct {
    volatile LONG      refs;
    const NtsProvider *candidates[NTP_NTS_SLOT_ATTEMPTS];
    size_t             candidate_count;
    const NtsProvider *provider;     // successful provider, or last attempted provider
    int                slot;
    int                attempts;
    NtpSourceResult    out;
    // Pending pin-rotation evidence from the successful attempt; only
    // meaningful when out.authMode == NTP_AUTH_ROTATED_PIN. The
    // aggregator promotes (persists) it only after the cycle passes the
    // trust gate with a continuous enrolled peer corroborating.
    NtsRotationPending rot;
    char               labelBuf[32]; // "NTS:<provider>", owned by this struct
} NtsCtx;

static void NtsCtx_AddRef(NtsCtx *ctx) {
    if (ctx) InterlockedIncrement(&ctx->refs);
}

static void NtsCtx_Release(NtsCtx *ctx) {
    if (ctx && InterlockedDecrement(&ctx->refs) == 0) free(ctx);
}

static DWORD WINAPI NtsWorkerProc(LPVOID param) {
    NtsCtx *ctx = (NtsCtx *)param;
    NtpSourceResult *r = &ctx->out;
    memset(r, 0, sizeof(*r));
    r->label = kNtsNoPinLabel;

    if (ctx->candidate_count == 0) {
        // No provider assigned. This slot fails; Ntp_Concur requires
        // two operator-diverse authenticated NTS samples.
        r->ok = 0;
        goto done;
    }

    for (size_t i = 0; i < ctx->candidate_count; i++) {
        const NtsProvider *p = ctx->candidates[i];
        if (p == NULL) continue;

        ctx->provider = p;
        ctx->attempts = (int)i + 1;

        // Copy the provider label into our owned buffer so the aggregator
        // can reference it after the worker returns.
        _snprintf(ctx->labelBuf, sizeof ctx->labelBuf, "NTS:%s",
                  p->label ? p->label : "?");
        ctx->labelBuf[sizeof ctx->labelBuf - 1] = 0;
        r->label = ctx->labelBuf;

        int64_t utc = 0, qpc = 0;
        uint32_t rtt = 0, rootErr = 0;
        if (Nts_FetchSampleEx(p, &utc, &qpc, &rtt, &rootErr, &ctx->rot)) {
            r->ok        = 1;
            r->ntpUtcMs  = utc;
            r->qpcAtT4   = qpc;
            r->rttMs     = rtt;
            r->rootErrMs = rootErr;
            Nts_ReportRtt(p, rtt);   // steers future draws toward near anchors
            r->offsetMs = utc;   // legacy field (overwritten with display value below)
            r->authMode = ctx->rot.pending ? NTP_AUTH_ROTATED_PIN
                                           : NTP_AUTH_ENROLLED_PIN;
            r->operatorFamily = p->operator_family;
            goto done;
        }

        if (i + 1 < ctx->candidate_count && ctx->candidates[i + 1]) {
            const NtsProvider *next = ctx->candidates[i + 1];
            Log_Append("ntp: NTS slot %d %s failed; retrying immediately with %s",
                       ctx->slot,
                       p->label ? p->label : "?",
                       next->label ? next->label : "?");
        }
    }

    r->ok = 0;
done:
    Ntp_WorkerFinished();
    NtsCtx_Release(ctx);
    return 0;
}

// --- Aggregator thread ---------------------------------------------------
//
// Spawns four core SNTP workers plus two NTS workers in parallel. Each
// slot receives a distinct primary candidate and a distinct replacement
// candidate; when the primary fails, the worker immediately tries its
// replacement inside the same polling cycle. Worker contexts are
// heap-owned and ref-counted, so if a broken network helper ever
// exceeds the cycle budget, the aggregator can detach that worker,
// publish the slot as failed, and clear g_running without returning
// while a stack frame is still in use. Completed workers are harvested
// normally and delegated to Ntp_Concur.

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------
//
// Every polling cycle appends one plain-text line to
// %APPDATA%\Lunar\audit.log. The file rotates to audit.log.1 at
// AUDIT_MAX_BYTES so the log can't grow unbounded. The format is line-
// oriented and greppable:
//
//   2026-04-21T12:34:56.789Z  OK    spread=  42ms  NIST:ok off= -12ms rtt= 85ms  PTB:ok off=  30ms rtt= 42ms  NICT:ok off=  10ms rtt=110ms  NTS:cloudflare:ok off=  0ms rtt=120ms
//   2026-04-21T12:35:01.000Z~ INOP  spread=   0ms  NIST:-- ...  PTB:ok ...  NICT:-- ...  NTS--:--
//
// "spread" here is the worst absolute deviation of any core source
// from the NTS anchor (in ms). On INOP cycles where NTS itself failed,
// spread is reported as 0.
//
// A trailing '~' on the timestamp marks a fallback stamp taken from
// the Windows system clock (used only when our own trusted clockwork
// cannot supply a time -- i.e. on an INOP cycle before the first ever
// concurrence). Stamps without '~' are from the disciplined clockwork.
//
// The logger never blocks the aggregator on I/O errors: a failed write
// is silently ignored. Losing a log line is preferable to stalling a
// polling cycle in a safety context.

#define AUDIT_MAX_BYTES (1 * 1024 * 1024)   // 1 MiB

static void AuditDir(wchar_t *out, size_t n) {
    if (!Lunar_AppDataPathW(out, n, NULL)) out[0] = 0;
}

static void AuditPath(wchar_t *out, size_t n) {
    if (!Lunar_AppDataPathW(out, n, L"audit.log")) out[0] = 0;
}

static void AuditRotateIfNeeded(void) {
    wchar_t path[MAX_PATH];
    AuditPath(path, MAX_PATH);
    if (path[0] == 0) return;

    WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;

    uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < AUDIT_MAX_BYTES) return;

    wchar_t rotated[MAX_PATH];
    _snwprintf_s(rotated, MAX_PATH, _TRUNCATE, L"%ls.1", path);
    DeleteFileW(rotated);       // ignore failure (may not exist)
    MoveFileW(path, rotated);   // ignore failure (next write will create fresh)
}

// Produce "YYYY-MM-DDTHH:MM:SS.mmmZ" from UTC ms since epoch.
// Buffer must be at least 25 bytes.
static void FormatIsoUtc(int64_t utcMs, char *out, size_t n) {
    time_t secs = (time_t)(utcMs / 1000);
    int    msec = (int)(utcMs % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    struct tm tm = { 0 };
    // gmtime_s is pure arithmetic: Unix epoch seconds to Y/M/D/h/m/s.
    // It does NOT read the system clock. Safe to use while remaining
    // independent of the Windows wall clock.
    gmtime_s(&tm, &secs);
    _snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}

// Timestamp helper for audit-log lines when no trusted UTC is yet
// available. We refuse to read the Windows system clock, so use the
// monotonic QPC counter since process start instead. The line will be
// marked with a '~' prefix so readers know this is a relative stamp,
// not real UTC.
//
//   T+000012.345s    -- 12.345 seconds since the clockwork was created
static void FormatRelativeStamp(char *out, size_t n) {
    static int64_t s_anchorQpc = 0;
    static int     s_anchored  = 0;
    if (!s_anchored) {
        s_anchorQpc = Clock_Qpc();
        s_anchored  = 1;
    }
    int64_t freq = Clock_QpcFreq();
    if (freq <= 0) freq = 1;
    int64_t ticks = Clock_Qpc() - s_anchorQpc;
    int64_t totalMs = (ticks * 1000LL + freq / 2) / freq;
    if (totalMs < 0) totalMs = 0;
    int64_t secs = totalMs / 1000;
    int     ms   = (int)(totalMs % 1000);
    _snprintf(out, n, "T+%06lld.%03ds", (long long)secs, ms);
}

// Grab the best timestamp we have: disciplined clock if OK, otherwise
// a QPC-relative "T+seconds since process start" marker. Returns 1 if
// the stamp is trusted wall-clock UTC.
static int BestLogStamp(char *out, size_t n) {
    int64_t utcMs = 0;
    if (Clock_NowUtcMs(&utcMs)) {
        FormatIsoUtc(utcMs, out, n);
        return 1;
    }
    FormatRelativeStamp(out, n);
    return 0;
}

// Open audit.log for appending (rotating first if oversized). Returns
// INVALID_HANDLE_VALUE when the path is unavailable; callers silently
// skip the line -- losing a log line is preferable to stalling a cycle.
static HANDLE AuditOpenAppend(void) {
    wchar_t dir[MAX_PATH];
    AuditDir(dir, MAX_PATH);
    if (dir[0] == 0) return INVALID_HANDLE_VALUE;

    AuditRotateIfNeeded();

    wchar_t path[MAX_PATH];
    AuditPath(path, MAX_PATH);
    if (path[0] == 0) return INVALID_HANDLE_VALUE;

    return CreateFileW(path,
                       FILE_APPEND_DATA,
                       FILE_SHARE_READ,
                       NULL,
                       OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

// Append one free-form NOTE line to the audit log, stamped the same way
// as cycle lines. Used for out-of-band security events (pin rotation
// acceptance) that must survive in the on-disk audit trail.
static void AuditNote(const char *text) {
    if (!text || !*text) return;
    HANDLE h = AuditOpenAppend();
    if (h == INVALID_HANDLE_VALUE) return;

    char stamp[32];
    int trusted = BestLogStamp(stamp, sizeof stamp);

    char line[512];
    int pos = _snprintf(line, sizeof(line), "%s%c NOTE  %s",
                        stamp, trusted ? ' ' : '~', text);
    if (pos < 0 || pos > (int)sizeof(line) - 2) pos = (int)sizeof(line) - 2;
    line[pos++] = '\r';
    line[pos++] = '\n';

    DWORD written = 0;
    WriteFile(h, line, (DWORD)pos, &written, NULL);
    CloseHandle(h);
}

static void AuditWrite(TrustState trust,
                       int64_t anchorErrMs,
                       const NtpSourceResult results[NTP_SOURCE_COUNT]) {
    HANDLE h = AuditOpenAppend();
    if (h == INVALID_HANDLE_VALUE) return;

    char stamp[32];
    int trusted = BestLogStamp(stamp, sizeof stamp);

    char line[768];
    int  pos = 0;
    pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                     "%s%c %-4s  anchorErr=%5lldms",
                     stamp,
                     trusted ? ' ' : '~',
                     (trust == TRUST_OK) ? "OK" : "INOP",
                     (long long)anchorErrMs);

    for (int i = 0; i < NTP_SOURCE_COUNT && pos < (int)sizeof(line) - 64; i++) {
        const NtpSourceResult *r = &results[i];
        const char *label = r->label ? r->label : "?";
        if (r->ok) {
            pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                             "  %s:ok off=%5lldms rtt=%4ums",
                             label,
                             (long long)r->offsetMs,
                             (unsigned)r->rttMs);
        } else {
            pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                             "  %s:-- off=    - rtt=   -",
                             label);
        }
    }
    if (pos < (int)sizeof(line) - 1) {
        line[pos++] = '\r';
        line[pos++] = '\n';
    } else {
        line[sizeof(line) - 2] = '\r';
        line[sizeof(line) - 1] = '\n';
        pos = sizeof(line);
    }

    DWORD written = 0;
    WriteFile(h, line, (DWORD)pos, &written, NULL);
    CloseHandle(h);
}

static DWORD WINAPI AggregatorProc(LPVOID param) {
    (void)param;

    // Four core workers (slots 0..3) plus two NTS workers (slots 4..5).
    WorkerCtx *core_ctx[NTP_CORE_COUNT] = { 0 };
    NtsCtx    *nts_ctx[NTP_NTS_COUNT] = { 0 };
    HANDLE     threads[NTP_SOURCE_COUNT] = { 0 };
    int        slot_done[NTP_SOURCE_COUNT] = { 0 };
    int        slot_overrun[NTP_SOURCE_COUNT] = { 0 };
    const char *slot_label[NTP_SOURCE_COUNT] = { 0 };
    const char *slot_host[NTP_SOURCE_COUNT] = { 0 };
    int        slot_attempts[NTP_SOURCE_COUNT] = { 0 };
    int        slot_timeout_ms[NTP_SOURCE_COUNT] = { 0 };
    char       nts_detached_label[NTP_NTS_COUNT][32] = { { 0 } };
    int        spawned = 0;

    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        slot_label[i] = "?";
        slot_host[i] = (i < NTP_CORE_COUNT) ? "(unassigned)" : "(NTS pool)";
        slot_timeout_ms[i] = (i < NTP_CORE_COUNT) ? NTP_TIMEOUT_MS
                                                  : NTS_SLOT_TIMEOUT_MS;
    }

    // Draw a fresh random subset of core-pool hosts for THIS cycle so
    // no single attacker can rely on always facing the same four
    // targets. Picks are striped across slots: first round = primary
    // sources, second round = immediate replacements.
    const NtpSource *chosenCore[NTP_CORE_COUNT * NTP_CORE_SLOT_ATTEMPTS] = { 0 };
    size_t nCore = PickCoreSources(chosenCore,
                                   NTP_CORE_COUNT * NTP_CORE_SLOT_ATTEMPTS);

    // Conditional in-cycle core retry: once the clock is already TRUST_OK the
    // 3/4-of-4 majority absorbs a one-off packet loss, so one attempt per core
    // slot suffices; while re-anchoring (not yet OK) keep the full replacement
    // retry. If a healthy single-attempt cycle does drop below OK, the next
    // cycle sees that here and restores the retry. The NTS slots ALWAYS keep
    // their full retry -- the authenticated pair is the trust floor.
    size_t coreAttempts =
        (Clock_Trust() >= TRUST_OK) ? 1u : (size_t)NTP_CORE_SLOT_ATTEMPTS;

    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        WorkerCtx *ctx = (WorkerCtx *)calloc(1, sizeof *ctx);
        if (!ctx) {
            Log_Append("ntp: core slot %d context allocation failed", i);
            continue;
        }
        ctx->refs = 1;
        core_ctx[i] = ctx;
        ctx->slot = i;
        for (size_t attempt = 0; attempt < coreAttempts; attempt++) {
            size_t idx = attempt * NTP_CORE_COUNT + (size_t)i;
            if (idx < nCore) {
                ctx->candidates[ctx->candidate_count++] = chosenCore[idx];
            }
        }
        ctx->src = ctx->candidate_count ? ctx->candidates[0] : NULL;
        memset(&ctx->out, 0, sizeof(ctx->out));
        ctx->out.label = ctx->src ? ctx->src->label : "?";
        slot_label[i] = ctx->out.label;
        slot_host[i] = ctx->src ? ctx->src->host : "(unassigned)";

        WorkerCtx_AddRef(ctx);
        Ntp_WorkerStarted();
        threads[i] = CreateThread(NULL, 0, WorkerProc, ctx, 0, NULL);
        if (threads[i]) {
            spawned++;
        } else {
            WorkerCtx_Release(ctx);  // drop worker reference
            Ntp_WorkerFinished();
            Log_Append("ntp: core slot %d CreateThread failed (err=%lu)",
                       i, (unsigned long)GetLastError());
        }
    }

    // Pick distinct NTS providers for primaries and replacements.
    // Even if one is somehow compromised (cert rotation fumbled, leaf
    // key disclosed, operator coerced), each visible NTS slot still
    // draws from independent pinned providers; Ntp_Concur rejects
    // divergent authenticated samples.
    const NtsProvider *chosenNts[NTP_NTS_COUNT * NTP_NTS_SLOT_ATTEMPTS] = { 0 };
    // While not currently trusted (acquiring, holdover, or reacquiring after a
    // suspend/resume), rotate to a fresh operator-diverse NTS pair rather than
    // reusing the sticky one -- so re-anchor never gambles on a bad or
    // stale-cookie provider. Sticky reuse resumes once TRUST_OK is held again.
    if (Clock_Trust() < TRUST_OK) Nts_ForceRepick();
    size_t nNts = Nts_PickProviders(chosenNts,
                                    NTP_NTS_COUNT * NTP_NTS_SLOT_ATTEMPTS);

    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        int slot = NTP_FIRST_NTS_SLOT + i;
        NtsCtx *ctx = (NtsCtx *)calloc(1, sizeof *ctx);
        if (!ctx) {
            Log_Append("ntp: NTS slot %d context allocation failed", slot);
            slot_label[slot] = kNtsNoPinLabel;
            continue;
        }
        ctx->refs = 1;
        nts_ctx[i] = ctx;
        ctx->slot = slot;
        for (size_t attempt = 0; attempt < NTP_NTS_SLOT_ATTEMPTS; attempt++) {
            size_t idx = attempt * NTP_NTS_COUNT + (size_t)i;
            if (idx < nNts) {
                ctx->candidates[ctx->candidate_count++] = chosenNts[idx];
            }
        }
        ctx->provider = ctx->candidate_count ? ctx->candidates[0] : NULL;
        if (ctx->provider) {
            _snprintf(nts_detached_label[i], sizeof nts_detached_label[i],
                      "NTS:%s", ctx->provider->label ? ctx->provider->label : "?");
            nts_detached_label[i][sizeof nts_detached_label[i] - 1] = 0;
            slot_label[slot] = nts_detached_label[i];
            slot_host[slot] = ctx->provider->host ? ctx->provider->host : "(NTS pool)";
        } else {
            _snprintf(nts_detached_label[i], sizeof nts_detached_label[i],
                      "%s", kNtsNoPinLabel);
            nts_detached_label[i][sizeof nts_detached_label[i] - 1] = 0;
            slot_label[slot] = nts_detached_label[i];
        }

        NtsCtx_AddRef(ctx);
        Ntp_WorkerStarted();
        threads[slot] = CreateThread(NULL, 0, NtsWorkerProc, ctx, 0, NULL);
        if (threads[slot]) {
            spawned++;
        } else {
            NtsCtx_Release(ctx);  // drop worker reference
            Ntp_WorkerFinished();
            Log_Append("ntp: NTS slot %d CreateThread failed (err=%lu)",
                       slot, (unsigned long)GetLastError());
        }
    }

    // Soft-cap total wall time at the NTS slot budget times the number
    // of same-cycle attempts. If a helper overruns that budget, mark
    // only the completed workers as harvestable and detach the rest;
    // their heap contexts remain alive until their worker reference exits.
    if (spawned > 0) {
        HANDLE hs[NTP_SOURCE_COUNT];
        int    nh = 0;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) hs[nh++] = threads[i];
        DWORD wait = WAIT_TIMEOUT;
        uint64_t waitStart = GetTickCount64();
        uint64_t shutdownDeadline = 0;
        for (;;) {
            wait = WaitForMultipleObjects((DWORD)nh, hs, TRUE, NTP_WAIT_SLICE_MS);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            if (wait != WAIT_TIMEOUT) break;

            uint64_t now = GetTickCount64();
            if (InterlockedCompareExchange(&g_shutdownRequested, 0, 0) != 0) {
                if (shutdownDeadline == 0) {
                    shutdownDeadline = now + NTP_SHUTDOWN_WORKER_GRACE_MS;
                    Log_Append("ntp: shutdown requested; giving workers %dms to finish",
                               NTP_SHUTDOWN_WORKER_GRACE_MS);
                }
                if (now >= shutdownDeadline) break;
            } else if (now - waitStart >= (uint64_t)NTP_CYCLE_TIMEOUT_MS) {
                break;
            }
        }
        if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED) {
            if (wait == WAIT_TIMEOUT) {
                if (InterlockedCompareExchange(&g_shutdownRequested, 0, 0) != 0) {
                    Log_Append("ntp: shutdown worker grace expired; detaching unfinished workers");
                } else {
                    Log_Append("ntp: cycle exceeded %dms retry budget; detaching overdue workers",
                               NTP_CYCLE_TIMEOUT_MS);
                }
            } else {
                Log_Append("ntp: worker wait failed (err=%lu); detaching unfinished workers",
                           (unsigned long)GetLastError());
            }
            for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
                if (!threads[i]) { slot_done[i] = 1; continue; }
                DWORD one = WaitForSingleObject(threads[i], 0);
                if (one == WAIT_OBJECT_0) slot_done[i] = 1;
                else slot_overrun[i] = 1;
            }
        } else {
            for (int i = 0; i < NTP_SOURCE_COUNT; i++)
                if (threads[i]) slot_done[i] = 1;
        }
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) CloseHandle(threads[i]);
    }
    for (int i = 0; i < NTP_SOURCE_COUNT; i++)
        if (!threads[i]) slot_done[i] = 1;

    NtpSourceResult snapshot[NTP_SOURCE_COUNT];
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        memset(&snapshot[i], 0, sizeof snapshot[i]);
        if (core_ctx[i] && slot_done[i] && !slot_overrun[i]) {
            snapshot[i] = core_ctx[i]->out;
            if (core_ctx[i]->src) slot_host[i] = core_ctx[i]->src->host;
            slot_attempts[i] = core_ctx[i]->attempts;
        } else {
            snapshot[i].label = slot_label[i] ? slot_label[i] : "?";
        }
    }
    for (int i = 0; i < NTP_NTS_COUNT;  i++) {
        int slot = NTP_FIRST_NTS_SLOT + i;
        memset(&snapshot[slot], 0, sizeof snapshot[slot]);
        if (nts_ctx[i] && slot_done[slot] && !slot_overrun[slot]) {
            snapshot[slot] = nts_ctx[i]->out;
            if (nts_ctx[i]->provider) slot_host[slot] = nts_ctx[i]->provider->host;
            slot_attempts[slot] = nts_ctx[i]->attempts;
        } else {
            snapshot[slot].label = slot_label[slot] ? slot_label[slot] : kNtsNoPinLabel;
        }
    }

    int64_t    bestUtcMs = 0;
    int64_t    bestQpc   = 0;
    int64_t    anchorErr = 0;
    int64_t    clusterUtc = 0, clusterQpc = 0;
    int        haveCluster = 0;
    TrustState trust = Ntp_Concur(snapshot, &bestUtcMs, &bestQpc, &anchorErr,
                                  &clusterUtc, &clusterQpc, &haveCluster);

    // Corroborated pin-rotation promotion: an NTS slot that completed
    // via a CA-valid leaf matching no stored pin (NTP_AUTH_ROTATED_PIN)
    // earns persistence ONLY when this cycle reached full TRUST_OK --
    // which by the gate rules means an operator-diverse, still-pinned
    // ENROLLED_PIN peer plus the core majority corroborated it. Collect
    // the evidence while the worker contexts are alive; the save and the
    // loud audit note happen after AuditWrite below, so the audit stamp
    // reflects the freshly disciplined clock.
    struct {
        int                valid;
        NtsRotationPending rot;
        const NtsProvider *provider;      // points into the static pool
        const char        *corroborator;  // other slot's operator family
    } promo[NTP_NTS_COUNT];
    memset(promo, 0, sizeof promo);
    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        int slot = NTP_FIRST_NTS_SLOT + i;
        if (!nts_ctx[i] || !slot_done[slot] || slot_overrun[slot]) continue;
        if (!snapshot[slot].ok ||
            snapshot[slot].authMode != NTP_AUTH_ROTATED_PIN) continue;
        if (!nts_ctx[i]->rot.pending || !nts_ctx[i]->provider) continue;
        if (trust != TRUST_OK) {
            Log_Append("ntp: NTS slot %d %s pin rotation NOT accepted (cycle did not pass the trust gate); nothing persisted",
                       slot, snapshot[slot].label ? snapshot[slot].label : "?");
            continue;
        }
        promo[i].valid = 1;
        promo[i].rot = nts_ctx[i]->rot;
        promo[i].provider = nts_ctx[i]->provider;
        promo[i].corroborator = "?";
        for (int j = 0; j < NTP_NTS_COUNT; j++) {
            int other = NTP_FIRST_NTS_SLOT + j;
            if (other == slot || !snapshot[other].ok) continue;
            if (snapshot[other].authMode != NTP_AUTH_ENROLLED_PIN) continue;
            if (snapshot[other].operatorFamily) {
                promo[i].corroborator = snapshot[other].operatorFamily;
            }
        }
    }

    // Rewrite snapshot[i].offsetMs to the MEANINGFUL display value:
    // this source's deviation from the cycle's trust anchor, projected
    // to a common QPC moment. The anchor is the Ntp_Concur result
    // (midpoint of NTS pair on full OK; surviving NTS on fallback);
    // otherwise fall back to the first successful source so the audit
    // log still carries informative per-source numbers.
    {
        int64_t qpcFreq = Clock_QpcFreq();
        if (qpcFreq <= 0) qpcFreq = 1;
        int64_t refQpc = 0, refUtc = 0;
        int have_ref = 0;
        if (trust == TRUST_OK) {
            // bestUtc/bestQpc is the cycle's authenticated NTS midpoint.
            refQpc = bestQpc;
            refUtc = bestUtcMs;
            have_ref = 1;
        } else {
            for (int i = NTP_FIRST_NTS_SLOT; i < NTP_SOURCE_COUNT; i++) {
                if (snapshot[i].ok) {
                    refQpc = snapshot[i].qpcAtT4;
                    refUtc = snapshot[i].ntpUtcMs;
                    have_ref = 1;
                    break;
                }
            }
            if (!have_ref) {
                for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
                    if (snapshot[i].ok) {
                        refQpc = snapshot[i].qpcAtT4;
                        refUtc = snapshot[i].ntpUtcMs;
                        have_ref = 1;
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
            if (!snapshot[i].ok || !have_ref) { snapshot[i].offsetMs = 0; continue; }
            int64_t dq = refQpc - snapshot[i].qpcAtT4;
            int64_t dms = (dq >= 0)
                ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
                : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
            int64_t projected = snapshot[i].ntpUtcMs + dms;
            snapshot[i].offsetMs = projected - refUtc;
        }
    }

    // Publish shared state. A completed NTS slot's label may point
    // into its worker context, while an overdue slot points into a
    // local detached-label buffer. Copy either form into file-scope
    // storage before releasing aggregator references.
    if (g_csInit) EnterCriticalSection(&g_cs);
    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        int slot = NTP_FIRST_NTS_SLOT + i;
        const char *lbl = snapshot[slot].label
                          ? snapshot[slot].label : kNtsNoPinLabel;
        _snprintf(g_ntsLabelPublished[i], sizeof g_ntsLabelPublished[i],
                  "%s", lbl);
        g_ntsLabelPublished[i][sizeof g_ntsLabelPublished[i] - 1] = 0;
        snapshot[slot].label = g_ntsLabelPublished[i];
    }
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) g_results[i] = snapshot[i];
    if (g_csInit) LeaveCriticalSection(&g_cs);

    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        if (core_ctx[i]) { WorkerCtx_Release(core_ctx[i]); core_ctx[i] = NULL; }
    }
    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        if (nts_ctx[i]) { NtsCtx_Release(nts_ctx[i]); nts_ctx[i] = NULL; }
    }

    // Inform the clockwork. If state is INOP the anchor is not updated
    // and Clock_NowUtcMs() will refuse to return a time (callers render
    // the big red INOP). On an INOP cycle whose unauthenticated cores
    // still clustered tightly, feed the widen-only watchdog -- it can
    // only ever inflate the published interval, never sustain a claim.
    Clock_OnPollCycle(trust, bestUtcMs, bestQpc, anchorErr);
    if (trust != TRUST_OK && haveCluster) {
        Clock_OnCoreWitness(clusterUtc, clusterQpc);
    }
    // g_lastSpreadMs is published by Ntp_Concur itself: the core cluster's
    // worst deviation from the authenticated anchor. It used to be
    // overwritten here with anchorErr, whose worst-NTS-RTT/2 term let a
    // transatlantic anchor pair silently veto the relaxed poll cadence.

    // One line to the audit log per cycle. Done after Clock_OnPollCycle
    // so the timestamp reflects the clockwork's *post-cycle* state
    // (disciplined if this cycle was OK, untrusted-fallback otherwise).
    AuditWrite(trust, anchorErr, snapshot);

    // Promote corroborated pin rotations collected above: persist the
    // new SPKI and leave a LOUD trace in both the in-memory event log
    // and the on-disk audit trail.
    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        if (!promo[i].valid) continue;
        const NtsProvider *p = promo[i].provider;
        const NtsRotationPending *rot = &promo[i].rot;
        PinStore_SavePin(PIN_ENDPOINT_NTS, p->label, p->host, rot->port,
                         p->operator_family, rot->spki, rot->spki_hex,
                         rot->not_before, rot->not_after,
                         rot->not_before_unix, rot->not_after_unix,
                         "rotation-corroborated");
        char msg[224];
        _snprintf(msg, sizeof msg,
                  "pin rotation accepted for %s: %.12s\xe2\x86\x92%.12s, "
                  "corroborated by %s",
                  p->host, rot->old_spki_hex, rot->spki_hex,
                  promo[i].corroborator);
        msg[sizeof msg - 1] = 0;
        Log_Append("ntp: PIN ROTATION ACCEPTED \xe2\x80\x94 %s", msg);
        AuditNote(msg);
    }

    // Mirror a detailed summary into the rolling in-memory log so the
    // "Log" menu item can show recent cycles without hitting disk.
    // One header line + one line per slot carrying offset (vs. cycle
    // consensus), RTT, and server-believed UTC so operators can see
    // exactly what each source returned.
    {
        int okCount   = 0;
        int ntsOk     = 0;
        int coreConcur= 0;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (snapshot[i].ok) okCount++;
        for (int i = NTP_FIRST_NTS_SLOT; i < NTP_SOURCE_COUNT; i++)
            if (snapshot[i].ok) ntsOk++;
        for (int i = 0; i < NTP_CORE_COUNT; i++) {
            if (!snapshot[i].ok) continue;
            int64_t off = snapshot[i].offsetMs;
            if (off < 0) off = -off;
            if (off <= CONCUR_THRESHOLD_MS) coreConcur++;
        }
        const char *verdictName =
              (trust == TRUST_OK) ? "TRUST_OK" : "TRUST_INOP";
        const char *gateDesc =
              (trust == TRUST_OK)
                  ? "2 operator-diverse NTS + >=3/4 core within +/-200ms"
            : haveCluster
                  ? "gate failed; core cluster reported to the widen-only watchdog"
                  : "need 2 operator-diverse NTS agreeing + >=3/4 core";
        Log_Append("ntp: cycle %s  concur=%d/%d core + %d/%d NTS  "
                   "anchorErr=%lldms  gate=%s",
                   verdictName,
                   coreConcur, NTP_CORE_COUNT,
                   ntsOk, NTP_NTS_COUNT,
                   (long long)anchorErr,
                   gateDesc);
        (void)okCount;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
            const char *lbl  = snapshot[i].label ? snapshot[i].label : "?";
            const char *host = slot_host[i] ? slot_host[i] : "(unassigned)";
            int attempts = slot_attempts[i];
            int timeoutMs = slot_timeout_ms[i];
            if (snapshot[i].ok) {
                char iso[40];
                FormatIsoUtc(snapshot[i].ntpUtcMs, iso, sizeof iso);
                Log_Append("  [%d] %-18s  %-22s  ok    attempts=%d  offset=%+lldms  "
                           "rtt=%ums  server_utc=%s",
                           i, lbl, host,
                           attempts,
                           (long long)snapshot[i].offsetMs,
                           (unsigned)snapshot[i].rttMs,
                           iso);
            } else if (slot_overrun[i]) {
                Log_Append("  [%d] %-18s  %-22s  FAIL  (worker exceeded "
                           "%dms cycle budget; detached so future syncs can run)",
                           i, lbl, host, NTP_CYCLE_TIMEOUT_MS);
            } else {
                Log_Append("  [%d] %-18s  %-22s  FAIL  (no valid reply after "
                           "%d attempt(s), ~%dms each \xe2\x80\x94" " DNS/timeout/blackhole/rate-limit)",
                           i, lbl, host,
                           attempts,
                           timeoutMs);
            }
        }
    }

    // Legacy accessors (About dialog etc.) only record on OK cycles.
    if (trust == TRUST_OK) {
        // g_offsetMs legacy meaning ("median offset") no longer
        // applies -- we don't read the system clock. Publish 0; the
        // per-source offsetMs values on the snapshot (deviation from
        // consensus) are what the About dialog displays now.
        InterlockedExchange64(&g_offsetMs,        0);
        InterlockedExchange64(&g_lastSuccessTick, (LONG64)GetTickCount64());
        InterlockedExchange64(&g_lastSuccessUtc,  (LONG64)bestUtcMs);
    }

    InterlockedExchange(&g_running, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Pure concurrence evaluator (exported; unit-tested in tests/test_core.c)
// ---------------------------------------------------------------------------
//
// Per ntp.h: slots NTP_FIRST_NTS_SLOT..NTP_SOURCE_COUNT-1 are the TWO
// NTS (authenticated) sources; they are the trust anchor. Slots
// 0..NTP_CORE_COUNT-1 are core (plain-SNTP) sources that corroborate.
//
// Full-OK rules:
//   * Both NTS slots ok, authenticated by pins (enrolled, or at most ONE
//     pending-rotation slot riding on a continuous enrolled peer), from
//     different operator families, AND they mutually agree within 200 ms
//     (projected to a common QPC). The anchor is the QPC-aligned
//     midpoint of the two NTS samples. Two pending-rotation slots have
//     no continuous corroborator and hard-fail the cycle.
//   * >= 3 of the 4 core sources must agree with the NTS midpoint to
//     within 200 ms. The 3-of-4 super-majority tolerates one national
//     outage while still requiring a strong majority.
//
// Rationale: the NTS reply is cryptographically authenticated, so an
// on-path adversary cannot forge it without defeating TLS 1.3 plus
// the enrolled leaf SPKI / scheduled Windows CA renewal. Plain SNTP
// packets, in contrast, are forgeable. Two independent NTS anchors +
// strong core majority raise the bar to "compromise two independent
// NTS operators AND a majority of national labs simultaneously".
//
// Each source captured its qpcAtT4 at a slightly different moment, so
// we cannot compare ntpUtcMs values directly. Instead we project all
// source UTC estimates onto a common QPC moment using the local QPC
// frequency, then threshold on the signed delta. This is pure
// arithmetic on monotonic QPC + server timestamps; the Windows system
// clock is never consulted.
//
// maxSpreadMs carries the largest absolute core deviation from the
// NTS anchor for logging, independent of the verdict.

// Project a source's ntpUtcMs onto `refQpc` (pure QPC arithmetic).
static int64_t ProjectUtcOntoQpc(int64_t sourceUtcMs,
                                 int64_t sourceQpc,
                                 int64_t refQpc,
                                 int64_t qpcFreq) {
    int64_t dq = refQpc - sourceQpc;
    int64_t dms = (dq >= 0)
        ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
        : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
    return sourceUtcMs + dms;
}

// Count core sources whose projected UTC agrees with (refUtc, refQpc)
// to within CONCUR_THRESHOLD_MS, and report the worst absolute
// deviation among successful cores.
static int CountCoreConcurring(const NtpSourceResult results[NTP_SOURCE_COUNT],
                               int64_t refUtc,
                               int64_t refQpc,
                               int64_t qpcFreq,
                               int64_t *outWorstAbs,
                               int64_t *outMedianConcurAbs,
                               int64_t *outWorstConcurAbs) {
    int concurring = 0;
    int64_t worst = 0;
    int64_t worstConcur = 0;
    int64_t concurAbs[NTP_CORE_COUNT];
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        const NtpSourceResult *r = &results[i];
        if (!r->ok) continue;
        int64_t projected = ProjectUtcOntoQpc(r->ntpUtcMs, r->qpcAtT4,
                                              refQpc, qpcFreq);
        int64_t delta = projected - refUtc;
        int64_t absD  = delta < 0 ? -delta : delta;
        if (absD > worst) worst = absD;
        if (absD <= CONCUR_THRESHOLD_MS) {
            concurAbs[concurring++] = absD;
            if (absD > worstConcur) worstConcur = absD;
        }
    }
    if (outWorstAbs) *outWorstAbs = worst;
    if (outWorstConcurAbs) *outWorstConcurAbs = worstConcur;
    // Median deviation of the CONCURRING cores from the reference: an
    // independent (if unauthenticated) witness of the anchor's error.
    // Median, not mean or max, so one outlier core can neither dominate
    // nor tighten it. Used widen-only by the anchor-error computation.
    if (outMedianConcurAbs) {
        int64_t med = 0;
        if (concurring > 0) {
            // insertion sort; at most NTP_CORE_COUNT (4) entries
            for (int i = 1; i < concurring; i++) {
                int64_t v = concurAbs[i];
                int j = i - 1;
                while (j >= 0 && concurAbs[j] > v) {
                    concurAbs[j + 1] = concurAbs[j];
                    j--;
                }
                concurAbs[j + 1] = v;
            }
            med = (concurring & 1)
                ? concurAbs[concurring / 2]
                : (concurAbs[concurring / 2 - 1] + concurAbs[concurring / 2]) / 2;
        }
        *outMedianConcurAbs = med;
    }
    return concurring;
}

// Core-only cluster for the widen-only watchdog. Among the successful
// core (plain-SNTP) slots, find the largest cluster that mutually agrees
// within `thresholdMs` after QPC-projection onto a common moment. If at
// least 3 core sources fall in one cluster, write the cluster-center
// value (projected to that moment) to *outUtc/*outQpc, the worst
// in-cluster deviation to *outWorst, and return the cluster size;
// otherwise return 0. The cluster carries NO trust: Clock_OnCoreWitness
// may only widen the published interval from it -- it never re-anchors,
// never clears a latch, never refreshes freshness.
static int CoreOnlyConsensus(const NtpSourceResult results[NTP_SOURCE_COUNT],
                             int64_t qpcFreq, int64_t thresholdMs,
                             int64_t *outUtc, int64_t *outQpc,
                             int64_t *outWorst) {
    int refIdx = -1;
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        if (results[i].ok) { refIdx = i; break; }
    }
    if (refIdx < 0) return 0;
    int64_t refQpc = results[refIdx].qpcAtT4;

    int64_t proj[NTP_CORE_COUNT];
    int     okSlot[NTP_CORE_COUNT];
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        okSlot[i] = results[i].ok ? 1 : 0;
        proj[i] = okSlot[i]
            ? ProjectUtcOntoQpc(results[i].ntpUtcMs, results[i].qpcAtT4,
                                refQpc, qpcFreq)
            : 0;
    }

    int     bestCount  = 0;
    int64_t bestCenter = 0;
    int64_t bestWorst  = 0;
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        if (!okSlot[i]) continue;
        int     count = 0;
        int64_t worst = 0;
        for (int j = 0; j < NTP_CORE_COUNT; j++) {
            if (!okSlot[j]) continue;
            int64_t d = proj[j] - proj[i];
            if (d < 0) d = -d;
            if (d <= thresholdMs) {
                count++;
                if (d > worst) worst = d;
            }
        }
        if (count > bestCount) {
            bestCount  = count;
            bestCenter = proj[i];
            bestWorst  = worst;
        }
    }

    if (bestCount < 3) return 0;
    if (outUtc)   *outUtc   = bestCenter;
    if (outQpc)   *outQpc   = refQpc;
    if (outWorst) *outWorst = bestWorst;
    return bestCount;
}

TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outAnchorErrMs,
                      int64_t *outClusterUtcMs,
                      int64_t *outClusterQpc,
                      int     *outHaveCluster) {
    if (outBestUtcMs)    *outBestUtcMs    = 0;
    if (outBestQpc)      *outBestQpc      = 0;
    if (outAnchorErrMs)  *outAnchorErrMs  = 0;
    if (outClusterUtcMs) *outClusterUtcMs = 0;
    if (outClusterQpc)   *outClusterQpc   = 0;
    if (outHaveCluster)  *outHaveCluster  = 0;

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) return TRUST_INOP;

    // Count successful, pin-authenticated NTS slots. ROTATED_PIN slots
    // (CA-valid leaf, no stored-pin match, outside the renewal window)
    // participate, but the continuity rule below restricts how they may
    // combine.
    int ntsOkCount = 0;
    int ntsRotated = 0;
    int ntsOkIdx[NTP_NTS_COUNT] = { -1, -1 };
    for (int i = 0; i < NTP_NTS_COUNT; i++) {
        int slot = NTP_FIRST_NTS_SLOT + i;
        if (results[slot].ok &&
            (results[slot].authMode == NTP_AUTH_ENROLLED_PIN ||
             results[slot].authMode == NTP_AUTH_ROTATED_PIN)) {
            if (results[slot].authMode == NTP_AUTH_ROTATED_PIN) ntsRotated++;
            ntsOkIdx[ntsOkCount++] = slot;
        }
    }

    // --- Both authenticated NTS anchors present --------------------------
    // When both NTS slots respond we must produce a full TRUST_OK or
    // hard-fail to INOP. A conflicting or duplicate-family NTS pair is
    // MORE alarming than absent NTS (it suggests an attack on, or
    // misconfiguration of, the authenticated layer), so it never earns
    // any weaker credit.
    if (ntsOkCount == 2) {
        // Continuity rule: a rotated pin is only trustworthy when a
        // CONTINUOUS enrolled pin from an independent operator vouches
        // for the cycle. Two rotated slots have no continuous
        // corroborator -- exactly the shape of a CA-level MITM against
        // both anchors -- so the cycle hard-fails rather than enrolling
        // either key.
        if (ntsRotated >= 2) return TRUST_INOP;

        const NtpSourceResult *a = &results[ntsOkIdx[0]];
        const NtpSourceResult *b = &results[ntsOkIdx[1]];
        const char *famA = a->operatorFamily ? a->operatorFamily : "";
        const char *famB = b->operatorFamily ? b->operatorFamily : "";
        if (famA[0] == 0 || famB[0] == 0 || strcmp(famA, famB) == 0) {
            return TRUST_INOP;
        }

        // Require mutual agreement within CONCUR_THRESHOLD_MS after
        // QPC-projection. If they disagree, one authenticated anchor is
        // lying; refuse to discipline the clock on a split verdict.
        int64_t bProjOnA = ProjectUtcOntoQpc(b->ntpUtcMs, b->qpcAtT4,
                                             a->qpcAtT4, qpcFreq);
        int64_t ntsDelta = bProjOnA - a->ntpUtcMs;  // b - a at a's qpc
        int64_t ntsAbs   = ntsDelta < 0 ? -ntsDelta : ntsDelta;
        // Publish the authenticated-pair spread so the poll scheduler can
        // require the two NTS anchors to agree TIGHTLY (not merely within the
        // 200 ms gate) before it relaxes the cadence.
        InterlockedExchange64(&g_lastNtsSpreadMs, (LONG64)ntsAbs);
        if (ntsAbs > CONCUR_THRESHOLD_MS) {
            if (outAnchorErrMs) *outAnchorErrMs = ntsAbs;
            return TRUST_INOP;
        }

        // Anchor = midpoint of the two NTS samples, expressed at slot a's
        // qpcAtT4. (a + bProjOnA) / 2 == a + ntsDelta/2.
        int64_t midUtc = a->ntpUtcMs + ntsDelta / 2;
        int64_t midQpc = a->qpcAtT4;

        int64_t worstCore = 0, medianCore = 0, spreadCore = 0;
        int coreConcur = CountCoreConcurring(results, midUtc, midQpc,
                                             qpcFreq, &worstCore, &medianCore,
                                             &spreadCore);
        if (coreConcur < 3) {
            if (outAnchorErrMs) *outAnchorErrMs = worstCore;
            InterlockedExchange64(&g_lastSpreadMs, (LONG64)worstCore);
            return TRUST_INOP;   // need >= 3 of 4
        }
        // The scheduler's convergence input: how far the CONCURRING cores
        // sit from the authenticated anchor (worst of them). Pure agreement
        // -- no RTT term -- so a far-away anchor cannot block relaxation
        // while every source still agrees.
        InterlockedExchange64(&g_lastSpreadMs, (LONG64)spreadCore);

        // --- Measured anchor uncertainty --------------------------------
        // The midpoint's worst-case error when the two anchors' errors have
        // opposite signs is bounded by the pair spread; when they share a
        // sign (one anchor faulty, or a correlated upstream error) it is
        // bounded by a single sample's asymmetry half-width plus what the
        // server itself claims. Sum -- not max -- of the halves stays a
        // bound in BOTH regimes:
        //   base = ntsAbs/2 + worstNtsRtt/2 + serverRootErr(floor 15 ms)
        // and the concurring cores' median deviation widens (never
        // tightens) it, covering correlated-NTS error the pair spread
        // cannot see (leap smear, shared upstream).
        {
            int64_t rttA = (int64_t)a->rttMs, rttB = (int64_t)b->rttMs;
            int64_t worstRtt = rttA > rttB ? rttA : rttB;
            int64_t rootA = (int64_t)a->rootErrMs, rootB = (int64_t)b->rootErrMs;
            int64_t worstRoot = rootA > rootB ? rootA : rootB;
            if (worstRoot < NTP_DISP_FLOOR_MS) worstRoot = NTP_DISP_FLOOR_MS;
            int64_t anchorErr = ntsAbs / 2 + worstRtt / 2 + worstRoot;
            if (medianCore > anchorErr) anchorErr = medianCore;
            if (outAnchorErrMs) *outAnchorErrMs = anchorErr;
        }

        if (outBestUtcMs) *outBestUtcMs = midUtc;
        if (outBestQpc)   *outBestQpc   = midQpc;
        return TRUST_OK;
    }

    // --- NTS unavailable: no trust, but maybe a witness -------------------
    // Fewer than two authenticated anchors can never produce a verdict.
    // If >= 3 core sources still mutually agree within the tight cluster
    // threshold, report the cluster so the caller can feed the widen-only
    // watchdog (Clock_OnCoreWitness): unauthenticated evidence may widen
    // the published interval, never sustain or tighten a claim.
    int64_t coreUtc = 0, coreQpc = 0, coreWorst = 0;
    int nCore = CoreOnlyConsensus(results, qpcFreq,
                                  CORE_CLUSTER_THRESHOLD_MS,
                                  &coreUtc, &coreQpc, &coreWorst);
    if (nCore >= 3) {
        if (outClusterUtcMs) *outClusterUtcMs = coreUtc;
        if (outClusterQpc)   *outClusterQpc   = coreQpc;
        if (outHaveCluster)  *outHaveCluster  = 1;
    }
    return TRUST_INOP;
}

// --- Public API ----------------------------------------------------------

void Ntp_Start(void) {
    int alreadyRunning = 0;
    int shuttingDown = 0;
    int createFailed = 0;
    DWORD createErr = 0;

    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (!Ntp_EnsureLifecycle()) {
        Log_Append("ntp: lifecycle init failed; cycle aborted");
        return;
    }

    EnterCriticalSection(&g_lifecycleCs);
    Ntp_CloseFinishedAggregatorLocked();
    if (g_shutdownRequested) {
        shuttingDown = 1;
    } else if (InterlockedCompareExchange(&g_running, 1, 0) != 0) {
        alreadyRunning = 1;
    } else {
        HANDLE th = CreateThread(NULL, 0, AggregatorProc, NULL, 0, NULL);
        if (th) {
            g_aggregatorThread = th;
        } else {
            createFailed = 1;
            createErr = GetLastError();
            InterlockedExchange(&g_running, 0);
        }
    }
    LeaveCriticalSection(&g_lifecycleCs);

    if (shuttingDown) {
        Log_Append("ntp: sync requested during shutdown; ignored");
        return;
    }
    if (alreadyRunning) {
        Log_Append("ntp: sync requested but a cycle is already in flight");
        return;
    }
    if (createFailed) {
        Log_Append("ntp: CreateThread failed (err=%lu); cycle aborted",
                   (unsigned long)createErr);
        return;
    }

    Log_Append("ntp: cycle start \xe2\x80\x94"
               " querying %d sources in parallel "
               "(%d core SNTP + %d NTS, replacement attempts %d/%d, "
               "core timeout %dms, NTS timeout %dms)",
               NTP_SOURCE_COUNT, NTP_CORE_COUNT, NTP_NTS_COUNT,
               NTP_CORE_SLOT_ATTEMPTS, NTP_NTS_SLOT_ATTEMPTS,
               NTP_TIMEOUT_MS, NTS_SLOT_TIMEOUT_MS);
    // A per-process constant: say it once, not on every cycle.
    static LONG s_poolLogged = 0;
    if (InterlockedCompareExchange(&s_poolLogged, 1, 0) == 0) {
        Log_Append("  core pool: %d curated stratum-1 servers "
                   "(national metrology / research labs); %d random picks per cycle",
                   (int)CORE_POOL_SIZE, NTP_CORE_COUNT);
    }
}

void Ntp_Shutdown(void) {
    if (!Ntp_EnsureLifecycle()) return;

    InterlockedExchange(&g_shutdownRequested, 1);
    Log_Append("ntp: shutdown requested");

    HANDLE th = NULL;
    EnterCriticalSection(&g_lifecycleCs);
    th = g_aggregatorThread;
    LeaveCriticalSection(&g_lifecycleCs);

    if (th) {
        DWORD wait = WaitForSingleObject(th, NTP_SHUTDOWN_AGGREGATOR_WAIT_MS);
        if (wait == WAIT_OBJECT_0) {
            EnterCriticalSection(&g_lifecycleCs);
            if (g_aggregatorThread == th) {
                CloseHandle(g_aggregatorThread);
                g_aggregatorThread = NULL;
            }
            LeaveCriticalSection(&g_lifecycleCs);
            Log_Append("ntp: aggregator stopped");
        } else {
            Log_Append("ntp: aggregator did not stop within %dms (wait=%lu, err=%lu)",
                       NTP_SHUTDOWN_AGGREGATOR_WAIT_MS,
                       (unsigned long)wait,
                       (unsigned long)GetLastError());
        }
    }

    if (g_workersIdleEvent) {
        DWORD wait = WaitForSingleObject(g_workersIdleEvent,
                                         NTP_SHUTDOWN_DETACHED_WAIT_MS);
        if (wait == WAIT_OBJECT_0) {
            Log_Append("ntp: all workers stopped");
        } else {
            Log_Append("ntp: workers still active after %dms; process exit will reclaim them",
                       NTP_SHUTDOWN_DETACHED_WAIT_MS);
        }
    }
}

int Ntp_IsSynced(void) {
    // Anchored at least once this run. The UI uses this only to choose
    // between NO SIGNAL (was synced, lost it) and ACQUIRING (never yet);
    // freshness itself is carried by the certainty interval.
    return g_lastSuccessTick != 0;
}

int64_t Ntp_OffsetMs(void) {
    return (int64_t)g_offsetMs;
}

int64_t Ntp_LastSyncUtcMs(void) {
    return (int64_t)g_lastSuccessUtc;
}

int64_t Ntp_LastSpreadMs(void) {
    return (int64_t)g_lastSpreadMs;
}

int64_t Ntp_LastNtsSpreadMs(void) {
    return (int64_t)g_lastNtsSpreadMs;
}

int Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]) {
    if (!out) return 0;
    if (!g_csInit) {
        memset(out, 0, sizeof(NtpSourceResult) * NTP_SOURCE_COUNT);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) out[i] = g_results[i];
    LeaveCriticalSection(&g_cs);
    int nok = 0;
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) if (out[i].ok) nok++;
    return nok;
}
