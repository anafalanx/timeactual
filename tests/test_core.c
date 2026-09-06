// test_core.c -- C unit tests for Lunar's pure engine logic.
//
// Exercises the engine libraries directly through their public headers;
// the .c files under test are linked in by tests/run_tests.py. The UI
// shell is Tcl/Tk (lunar.tcl) and is covered separately via `z check`.

#include "../src/clock.h"
#include "../src/ntp.h"
#include "../src/logbuf.h"
#include "../src/tz.h"
#include "../src/tz_winmap.h"
#include "../src/siv.h"
#include "../src/nts.h"
#include "../src/nts_ke.h"
#include "../src/nts_ef.h"
#include "../src/app_paths.h"
#include "../src/tzif.h"
#include "../src/dns.h"
#include "../src/netutil.h"
#include "../src/pin_store.h"
#include "../src/cert_verify_win.h"
#include "../src/update_check.h"

#include <wincrypt.h>   // CryptProtectData for the pin-store migration test

#include <stdio.h>
#include <wchar.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) do {                                                \
    if (expr) { g_pass++; }                                             \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); \
    }                                                                   \
} while (0)

#define CHECK_EQ_INT(a, b) do {                                         \
    long long _a = (long long)(a), _b = (long long)(b);                 \
    if (_a == _b) { g_pass++; }                                         \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s == %s  (%lld vs %lld)\n",       \
                __FILE__, __LINE__, #a, #b, _a, _b);                    \
    }                                                                   \
} while (0)

#define CHECK_EQ_STR(a, b) do {                                         \
    const char *_a = (a), *_b = (b);                                    \
    if (_a && _b && strcmp(_a, _b) == 0) { g_pass++; }                  \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n",   \
                __FILE__, __LINE__, #a, #b,                             \
                _a ? _a : "(null)", _b ? _b : "(null)");                \
    }                                                                   \
} while (0)

// ---------------------------------------------------------------------------
// LE byte writers (trivial but catches a bad refactor)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// WAV builder: RIFF header + PCM properties
// ---------------------------------------------------------------------------

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// ---------------------------------------------------------------------------
// Timezone abbreviation table
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// IANA time-zone resolver (tz.c + tzif.c + embedded tzdata)
// ---------------------------------------------------------------------------

#include "../src/tz.h"

static int64_t make_utc_ms(int y, int mo, int d, int h, int mi, int s) {
    // Build a UTC millisecond value without any OS calls.  Walk
    // through years; within the year use the zone-neutral
    // days-per-month table.
    static const int md[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++) {
        int leap = ((yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0));
        days += leap ? 366 : 365;
    }
    int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
    for (int i = 1; i < mo; i++) {
        days += md[i - 1];
        if (i == 2 && leap) days += 1;
    }
    days += (d - 1);
    int64_t secs = days * 86400 + (int64_t)h * 3600
                 + (int64_t)mi * 60 + s;
    return secs * 1000;
}

static void test_tz_lookup(void) {
    // UTC is always present, always index 0, offset 0, abbr "UTC".
    CHECK_EQ_INT(Tz_FindByName("UTC"), TZ_ID_UTC);
    CHECK(Tz_Count() > 1);
    CHECK_EQ_STR(Tz_Name(TZ_ID_UTC), "UTC");

    TzifLocal tl;
    int64_t t = make_utc_ms(2026, 7, 1, 12, 0, 0);   // 2026-07-01 12:00:00Z
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 0);
    CHECK_EQ_STR(tl.abbr, "UTC");
    CHECK_EQ_INT(tl.hour, 12);

    // Europe/Berlin in summer: CEST +02:00.
    TzId berlin = Tz_FindByName("Europe/Berlin");
    CHECK(berlin != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(berlin, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 7200);
    CHECK_EQ_STR(tl.abbr, "CEST");
    CHECK_EQ_INT(tl.isDst, 1);
    CHECK_EQ_INT(tl.hour, 14);  // 12Z + 2h

    // Europe/Berlin in winter: CET +01:00.
    int64_t tw = make_utc_ms(2026, 1, 15, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(berlin, tw, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 3600);
    CHECK_EQ_STR(tl.abbr, "CET");
    CHECK_EQ_INT(tl.isDst, 0);
    CHECK_EQ_INT(tl.hour, 13);

    // America/New_York in winter: EST -05:00.
    TzId ny = Tz_FindByName("America/New_York");
    CHECK(ny != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(ny, tw, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, -5 * 3600);
    CHECK_EQ_STR(tl.abbr, "EST");
    CHECK_EQ_INT(tl.hour, 7);
    CHECK_EQ_INT(tl.mday, 15);

    // America/New_York in summer: EDT -04:00.
    CHECK(Tz_LocalFromUtcMs(ny, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, -4 * 3600);
    CHECK_EQ_STR(tl.abbr, "EDT");
    CHECK_EQ_INT(tl.isDst, 1);

    // Asia/Tokyo has no DST: JST +09:00 year round.
    TzId tok = Tz_FindByName("Asia/Tokyo");
    CHECK(tok != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(tok, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 9 * 3600);
    CHECK_EQ_STR(tl.abbr, "JST");
    CHECK_EQ_INT(tl.isDst, 0);

    // Unknown names return invalid.
    CHECK_EQ_INT(Tz_FindByName("Bogus/Fictional"), TZ_ID_INVALID);
    CHECK_EQ_INT(Tz_FindByName(""),                TZ_ID_INVALID);
}

// ---------------------------------------------------------------------------
// Hit testing on hour markers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Armed persistence round-trip
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// NTP packet parsing -- run the byte math the way ntp.c does, against a
// synthetically crafted response with known timestamps, and verify the
// offset comes out to ~0.
// ---------------------------------------------------------------------------
//
// ntp.c parses inline so we re-derive the math here. This catches a
// refactor of the byte offsets or endianness.

static void test_ntp_timestamp_math(void) {
    // Simulate: client sent at t=1_000_000 ms since NTP epoch, server
    // received at 1_000_050, sent at 1_000_051, client got reply at
    // 1_000_100. True offset between client and server = 0 here.
    //
    // In ntp.c:
    //   t2_ms = (t2_s - epoch_delta)*1000 + (frac * 1000 >> 32)
    //   offset = ((t2 - t1) + (t3 - t4)) / 2 (in ms)
    uint64_t epoch = 2208988800ULL;
    uint32_t t2_s = (uint32_t)(epoch + 1000);
    uint32_t t3_s = t2_s;
    // 0.5 sec fractional part: frac = 2^31
    uint32_t t2_frac = 0x80000000u;
    uint32_t t3_frac = 0x80000000u;

    int64_t t2_ms = ((int64_t)t2_s - (int64_t)epoch) * 1000
                  + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    int64_t t3_ms = ((int64_t)t3_s - (int64_t)epoch) * 1000
                  + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    // 1000 seconds + 0.5 sec = 1_000_500 ms
    CHECK_EQ_INT(t2_ms, 1000500);
    CHECK_EQ_INT(t3_ms, 1000500);

    // Client t1=1_000_400 ms, t4=1_000_600 ms. Offset = ((500 - 400) +
    // (500 - 600))/2 = 0.
    int64_t t1 = 1000400, t4 = 1000600;
    int64_t off = ((t2_ms - t1) + (t3_ms - t4)) / 2;
    CHECK_EQ_INT(off, 0);

    // Shift server forward by 250 ms.
    t2_ms += 250; t3_ms += 250;
    off = ((t2_ms - t1) + (t3_ms - t4)) / 2;
    CHECK_EQ_INT(off, 250);
}

// ---------------------------------------------------------------------------
// NTP header validation: build synthetic packets and run them through the
// same byte-accessors / bit-masks used by ntp.c.
// ---------------------------------------------------------------------------

static int ntp_header_accepts(uint8_t li_vn_mode, uint8_t stratum) {
    uint8_t li   = (li_vn_mode >> 6) & 0x3;
    uint8_t vn   = (li_vn_mode >> 3) & 0x7;
    uint8_t mode =  li_vn_mode       & 0x7;
    if (li == 3)                         return 0;
    if (vn != 3 && vn != 4)              return 0;
    if (mode != 4)                       return 0;
    if (stratum == 0 || stratum >= 16)   return 0;
    return 1;
}

static void test_ntp_header_validation(void) {
    // Good: LI=0, VN=4, Mode=4, stratum=2
    CHECK_EQ_INT(ntp_header_accepts(0x24, 2), 1);
    // LI=3 (alarm/unsynced)
    CHECK_EQ_INT(ntp_header_accepts(0xE4, 2), 0);
    // VN=2 (ancient)
    CHECK_EQ_INT(ntp_header_accepts(0x14, 2), 0);
    // Mode=3 (client — this is an echo of our own packet, reject)
    CHECK_EQ_INT(ntp_header_accepts(0x23, 2), 0);
    // Stratum=0 (kiss-of-death: DENY/RATE/etc.)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 0), 0);
    // Stratum=16 (unsynchronized)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 16), 0);
    // Stratum=15 (edge, valid)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 15), 1);
    // VN=3 also acceptable (server speaks older version)
    CHECK_EQ_INT(ntp_header_accepts(0x1C, 2), 1);
}

// ---------------------------------------------------------------------------
// Disciplined clock: anchor + PLL behaviour.
// ---------------------------------------------------------------------------
//
// Clock_Init/OnSyncedNtpUtc/NowUtcMs are tied to Windows time internally,
// but the core math can be exercised by feeding synthetic QPC values and
// reading Clock_RatePpm() / Clock_OffsetMs(). We don't cover the
// ProjectLocked() slew curve here (hard to script against the real QPC);
// we cover the PLL convergence and the snap threshold.

static void test_clock_discipline(void) {
    // Redirect APPDATA so we don't clobber the user's discipline.dat.
    wchar_t scratchW[MAX_PATH + 32];
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    _snwprintf(scratchW, MAX_PATH + 32, L"%ls\\build\\test_scratch", cwd);
    _wmkdir(scratchW);
    wchar_t envsetW[MAX_PATH + 48];
    _snwprintf(envsetW, MAX_PATH + 48, L"APPDATA=%ls", scratchW);
    _wputenv(envsetW);

    // Kill any stale discipline file from a previous run.
    wchar_t discPath[MAX_PATH];
    _snwprintf(discPath, MAX_PATH, L"%ls\\Lunar\\discipline.dat", scratchW);
    _wremove(discPath);

    Clock_Init();

    // Not disciplined yet: must return 0 rate.
    CHECK_EQ_INT(Clock_IsDisciplined(), 0);
    CHECK_EQ_INT(Clock_RatePpm(), 0);

    // Simulate an NTP sample. Use the REAL QPC so ProjectLocked's deltas
    // are consistent with subsequent samples we'll feed it.
    int64_t qpcFreq;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpcFreq = f.QuadPart; }

    int64_t t0_utc;
    { FILETIME ft; GetSystemTimeAsFileTime(&ft);
      ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
      t0_utc = (int64_t)(u.QuadPart / 10000LL) - 11644473600000LL; }
    int64_t t0_qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(t0_utc, t0_qpc);
    CHECK_EQ_INT(Clock_IsDisciplined(), 1);
    CHECK_EQ_INT(Clock_RatePpm(), 0);   // only one sample => rate undefined

    // Second sample: pretend 1 hour of QPC elapsed, and the server says
    // we drifted +100 ms. Observed rate correction = +100 ms / 3.6e6 ms
    // = +27.7 ppm. PI Ki=1/8, starting from 0 => new rate ~= 3 ppm.
    int64_t oneHourQpc = t0_qpc + qpcFreq * 3600;
    int64_t t1_utc     = t0_utc + 3600LL * 1000 + 100;  // server says +100ms
    Clock_OnSyncedNtpUtc(t1_utc, oneHourQpc);
    int32_t r1 = Clock_RatePpm();
    CHECK(r1 >= 2 && r1 <= 5);   // 27.7 * (1/8) = 3.5

    // Third sample another hour later: re-anchor means the clock's
    // anchor is now (oneHourQpc, t1_utc) with rate r1. Over the next
    // hour with ~3 ppm, projection accounts for ~10.8 ms; server
    // reports another +100 ms total drift, leaving ~89 ms residual.
    // observed_ppm ~= 24.7, delta ~= 3 ppm, so r2 ~= r1 + 3.
    int64_t twoHourQpc = t0_qpc + qpcFreq * 7200;
    int64_t t2_utc     = t1_utc + 3600LL * 1000 + 100;
    Clock_OnSyncedNtpUtc(t2_utc, twoHourQpc);
    int32_t r2 = Clock_RatePpm();
    CHECK(r2 > r1);            // PI is converging toward ~28 ppm
    CHECK(r2 < 20);            // gently, bounded by per-cycle clamp

    // Clock_Shutdown must persist r2 and a >30-day-old timestamp must
    // be rejected on reload. First shutdown+reload with fresh timestamp
    // should restore r2 as the bootstrap.
    Clock_Shutdown();

    // Reset in-memory state (simulate app restart) and verify the
    // bootstrap rate is loaded.
    Clock_Init();
    CHECK_EQ_INT(Clock_IsDisciplined(), 0);     // not disciplined until first sync
    // First sync after restart: bootstrap rate should be applied.
    int64_t rs_qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(t0_utc + 100000, rs_qpc);
    CHECK_EQ_INT(Clock_RatePpm(), r2);           // loaded bootstrap
}

static void test_clock_display_states(void) {
    // Redirect APPDATA so we don't clobber the user's discipline.dat.
    wchar_t scratchW[MAX_PATH + 32];
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    _snwprintf(scratchW, MAX_PATH + 32, L"%ls\\build\\test_scratch", cwd);
    _wmkdir(scratchW);
    wchar_t envsetW[MAX_PATH + 48];
    _snwprintf(envsetW, MAX_PATH + 48, L"APPDATA=%ls", scratchW);
    _wputenv(envsetW);

    Clock_Init();

    // Never anchored this run: hard INOP, nothing renderable.
    ClockDisplay d0;
    Clock_GetDisplay(&d0);
    CHECK_EQ_INT(d0.state, TRUST_INOP);
    CHECK(d0.utcMs == 0);
    CHECK(d0.lastSyncUtcMs == 0);
    int64_t display = 0;
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 0);

    int64_t trustedUtc = make_utc_ms(2026, 4, 25, 12, 0, 0);
    Clock_OnPollCycle(TRUST_OK, trustedUtc, Clock_Qpc(), 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);

    uint64_t gen1 = 0;
    CHECK_EQ_INT(Clock_ReadDisplayTime(&display, &gen1), 1);
    CHECK(gen1 != 0);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen1), 1);
    CHECK(display >= trustedUtc);

    Clock_OnPollCycle(TRUST_OK, display, Clock_Qpc(), 0);
    uint64_t gen2 = 0;
    CHECK_EQ_INT(Clock_ReadDisplayTime(&display, &gen2), 1);
    CHECK(gen2 != 0);
    CHECK(gen2 != gen1);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen1), 0);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen2), 1);

    // A fresh anchor claims a tight bound (base 200 ms + negligible age).
    ClockDisplay d1;
    Clock_GetDisplay(&d1);
    CHECK_EQ_INT(d1.state, TRUST_OK);
    CHECK(d1.boundMs >= 200 && d1.boundMs < 300);
    CHECK(d1.lastSyncUtcMs >= trustedUtc);

    // Staleness: with no corroborating cycle for >CYCLE_STALE_AFTER_MS
    // (660 s, sized above the relaxed 10-min poll ceiling + cycle budget)
    // the display derives to honest HOLDOVER -- still a running time, never
    // dark, same projection basis (the generation is untouched).
    Clock_TestAgeLastCycle(661000);
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 1);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen2), 1);

    // The bound grows with anchor age: 200 ppm over the total 661 s + 1 h
    // aged so far = ~852 ms of growth on the (default 200 ms) base.
    Clock_TestAgeAnchor(3600000);
    ClockDisplay d2;
    Clock_GetDisplay(&d2);
    CHECK_EQ_INT(d2.state, TRUST_HOLDOVER);
    CHECK(d2.boundMs >= 1040 && d2.boundMs <= 1070);
    CHECK(d2.lastSyncAgeMs >= 3600000);

    // Continuity break (suspend/resume): REACQUIRING carries only the
    // last verified reading, never a running projection.
    Clock_OnContinuityBroken("unit test");
    ClockDisplay d3;
    Clock_GetDisplay(&d3);
    CHECK_EQ_INT(d3.state, TRUST_REACQUIRING);
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 0);
    CHECK(d3.lastSyncUtcMs >= trustedUtc);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen2), 0);

    // Unauthenticated evidence cannot end reacquisition: neither an INOP
    // cycle nor a core-cluster witness may repair broken continuity...
    Clock_OnPollCycle(TRUST_INOP, 0, 0, 0);
    Clock_OnCoreWitness(trustedUtc, Clock_Qpc());
    CHECK_EQ_INT(Clock_Trust(), TRUST_REACQUIRING);

    // ...only a full authenticated cycle re-anchors and restores display.
    int64_t reUtc = trustedUtc + 5000;
    Clock_OnPollCycle(TRUST_OK, reUtc, Clock_Qpc(), 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 1);
    CHECK(display >= reUtc);

    // Hard INOP latch (unrenderable fault, e.g. tz conversion failure):
    // no time at all until a corroborating cycle clears it.
    uint64_t gen3 = 0;
    CHECK_EQ_INT(Clock_ReadDisplayTime(&display, &gen3), 1);
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen3), 1);
    Clock_TripInop("unit test");
    CHECK_EQ_INT(Clock_DisplayGenerationIsCurrent(gen3), 0);
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_INOP);
    Clock_OnPollCycle(TRUST_OK, reUtc, Clock_Qpc(), 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);
    CHECK_EQ_INT(Clock_NowUtcMs(&display), 1);
}

// ---------------------------------------------------------------------------
// Adversarial discipline-loop tests (sync-evaluation.md C7)
// ---------------------------------------------------------------------------
//
// Feed Clock_OnSyncedNtpUtc / Clock_OnPollCycle crafted (utc, qpc) series
// and assert the PLL stays bounded and converges. The rate/anchor math is
// driven entirely by the qpc/utc values we pass in, so these are
// deterministic regardless of wall-clock or real QPC. (The display lease
// uses real QPC, but every test completes well inside the 90 s lease.)
//
// RATE_CLAMP_PPM (200) and PER_CYCLE_RATE_CLAMP_PPM (20) are file-local to
// clock.c, so the literals are repeated here with naming comments.

// Point APPDATA at a scratch dir and delete any persisted discipline.dat so
// each test starts from a known rate=0 bootstrap.
static void clock_test_reset_appdata(void) {
    wchar_t scratchW[MAX_PATH + 32];
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    _snwprintf(scratchW, MAX_PATH + 32, L"%ls\\build\\test_scratch", cwd);
    _wmkdir(scratchW);
    wchar_t envsetW[MAX_PATH + 48];
    _snwprintf(envsetW, MAX_PATH + 48, L"APPDATA=%ls", scratchW);
    _wputenv(envsetW);
    wchar_t discPath[MAX_PATH];
    _snwprintf(discPath, MAX_PATH, L"%ls\\Lunar\\discipline.dat", scratchW);
    _wremove(discPath);
}

static int64_t clock_test_qpc_freq(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return f.QuadPart ? f.QuadPart : 1;
}

// A steady frequency error must be learned by the PI loop and must
// converge -- not oscillate or run away. Per-cycle and absolute clamps
// must hold at every step.
static void test_clock_drift_convergence(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t f   = clock_test_qpc_freq();
    int64_t utc = make_utc_ms(2026, 5, 1, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);            // first anchor, rate still 0

    const int driftPpm = 45;
    int32_t prev = Clock_RatePpm();
    for (int k = 0; k < 80; k++) {
        int64_t dQ   = f * 3600;               // 1 h of QPC ticks
        int64_t real = dQ * 1000 / f;          // == 3 600 000 ms
        int64_t srv  = real + (real * driftPpm) / 1000000;
        qpc += dQ;
        utc += srv;
        Clock_OnSyncedNtpUtc(utc, qpc);
        int32_t r = Clock_RatePpm();
        CHECK(r - prev <= 20 && prev - r <= 20);   // per-cycle change clamp
        CHECK(r <= 200 && r >= -200);              // absolute clamp
        prev = r;
    }
    // Integer truncation of the I-step stalls within ~8 ppm of target; the
    // rate must have climbed most of the way to +45 without overshooting.
    int32_t fin = Clock_RatePpm();
    CHECK(fin >= driftPpm - 9 && fin <= driftPpm + 1);
}

// A single gate-passing cycle must never swing the rate by more than the
// per-cycle clamp, and sustained extreme error must saturate at the
// absolute clamp, never beyond -- in both directions (S7 "pulse" defense).
// Driven at the relaxed 600 s cadence, where the full 20 ppm/cycle clamp
// applies (shorter intervals get proportionally less; see
// test_clock_rate_clamp_scales_with_interval).
static void test_clock_rate_clamps(void) {
    int64_t f = clock_test_qpc_freq();

    // Positive saturation: server runs 5 s ahead every 600 s cycle.
    clock_test_reset_appdata();
    Clock_Init();
    int64_t utc = make_utc_ms(2026, 5, 2, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);
    int32_t prev = Clock_RatePpm();
    for (int k = 0; k < 15; k++) {
        qpc += f * 600;                 // 600 s
        utc += 600000 + 5000;           // +5 s of error per cycle
        Clock_OnSyncedNtpUtc(utc, qpc);
        int32_t r = Clock_RatePpm();
        CHECK(r - prev <= 20);          // per-cycle change clamp
        CHECK(r <= 200);                // absolute clamp, never exceeded
        prev = r;
    }
    CHECK_EQ_INT(Clock_RatePpm(), 200); // fully saturated after >=10 cycles

    // Negative saturation: server falls 5 s behind every 600 s cycle.
    clock_test_reset_appdata();
    Clock_Init();
    utc = make_utc_ms(2026, 5, 3, 0, 0, 0);
    qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);
    prev = Clock_RatePpm();
    for (int k = 0; k < 15; k++) {
        qpc += f * 600;
        utc += 600000 - 5000;           // -5 s of error per cycle
        Clock_OnSyncedNtpUtc(utc, qpc);
        int32_t r = Clock_RatePpm();
        CHECK(prev - r <= 20);
        CHECK(r >= -200);
        prev = r;
    }
    CHECK_EQ_INT(Clock_RatePpm(), -200);
}

// Symmetric round-trip jitter must not accumulate into a runaway rate
// (sync-evaluation.md C3): zero true drift with +/-100 ms alternating error
// over hourly cycles must leave the rate near zero, far from saturation.
static void test_clock_jitter_rejection(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t f   = clock_test_qpc_freq();
    int64_t utc = make_utc_ms(2026, 5, 4, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);
    int32_t worst = 0;
    for (int k = 0; k < 40; k++) {
        qpc += f * 3600;
        int64_t jitter = (k & 1) ? 100 : -100;
        utc += 3600000 + jitter;        // zero-mean drift, +/-100 ms noise
        Clock_OnSyncedNtpUtc(utc, qpc);
        int32_t r = Clock_RatePpm();
        int32_t a = r < 0 ? -r : r;
        if (a > worst) worst = a;
    }
    CHECK(worst <= 30);                 // bounded, nowhere near +/-200
}

// The local-oscillator-fault gate trips INOP when an otherwise-concurring
// cycle disagrees with our projection by >200 ms, and the escape hatch
// force-snaps after LOCAL_FAULT_ESCAPE_N consecutive faults (known-issues
// #3 / S4). Drives Clock_OnPollCycle -- the full verdict path.
static void test_clock_fault_gate_and_escape(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t utc = make_utc_ms(2026, 5, 5, 0, 0, 0);
    int64_t qpc = Clock_Qpc();

    // First concurrence-valid cycle: accepted as-is (no prior anchor).
    Clock_OnPollCycle(TRUST_OK, utc, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);
    CHECK_EQ_INT(Clock_IsDisciplined(), 1);

    // Cycles at the SAME qpc (zero elapsed -> projection == anchor utc)
    // whose agreed time is +500 ms off: each is a corroborated
    // disagreement. Fail-honest: the display keeps running as HOLDOVER
    // with the bound inflated to cover the disagreement, and after the
    // Nth consecutive fault the consensus wins (forced re-anchor).
    int64_t bad = utc + 500;
    int64_t disp = 0;
    Clock_OnPollCycle(TRUST_OK, bad, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);        // fault 1/3
    CHECK_EQ_INT(Clock_NowUtcMs(&disp), 1);             // still displaying
    {
        ClockDisplay df;
        Clock_GetDisplay(&df);
        CHECK(df.boundMs >= 500);                       // covers the 500 ms
    }
    Clock_OnPollCycle(TRUST_OK, bad, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);        // fault 2/3
    Clock_OnPollCycle(TRUST_OK, bad, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);              // forced re-anchor
    {
        ClockDisplay df;
        Clock_GetDisplay(&df);
        CHECK(df.boundMs < 500);                        // inflation cleared
    }

    // After the snap the anchor moved to `bad`; a matching cycle agrees.
    Clock_OnPollCycle(TRUST_OK, bad, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);

    // Evidence rule: a disagreement the sample's OWN uncertainty covers is
    // not evidence against the projection (a congested cycle: RTTs of
    // seconds, anchorErr in the hundreds of ms). Such cycles hold with the
    // bound inflated and never count toward the forced re-anchor -- three
    // in a row must NOT snap.
    int64_t noisy = bad + 500;
    for (int i = 0; i < 3; i++) Clock_OnPollCycle(TRUST_OK, noisy, qpc, 600);
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);          // held, not snapped
    {
        ClockDisplay df;
        Clock_GetDisplay(&df);
        CHECK(df.boundMs >= 500);                         // honest inflation
    }
    Clock_OnPollCycle(TRUST_OK, bad, qpc, 0);             // agreement resumes
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);

    // Consistency rule: a streak that flips sign restarts. +500, -500, +500
    // is not three corroborating faults; it takes two more +500 to snap.
    Clock_OnPollCycle(TRUST_OK, bad + 500, qpc, 0);       // streak 1 (+)
    Clock_OnPollCycle(TRUST_OK, bad - 500, qpc, 0);       // restart (-)
    Clock_OnPollCycle(TRUST_OK, bad + 500, qpc, 0);       // restart (+) -> 1
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);          // no snap yet
    Clock_OnPollCycle(TRUST_OK, bad + 500, qpc, 0);       // 2
    CHECK_EQ_INT(Clock_Trust(), TRUST_HOLDOVER);
    Clock_OnPollCycle(TRUST_OK, bad + 500, qpc, 0);       // 3 -> snap
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);
}

// The per-cycle rate clamp scales with the measurement interval: at a
// 60 s cadence a few ms of network noise reads as ~100 ppm and must not
// swing the rate by the full 20 ppm the 10-minute cadence is allowed.
static void test_clock_rate_clamp_scales_with_interval(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t f   = clock_test_qpc_freq();
    int64_t utc = make_utc_ms(2026, 5, 7, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);
    // Second sync at +60 s with a +8 ms residual: observed 133 ppm, gain
    // 1/8 -> 16 ppm requested, but the 60 s clamp allows only 2.
    qpc += 60 * f; utc += 60000 + 8;
    Clock_OnSyncedNtpUtc(utc, qpc);
    {
        int32_t r = Clock_RatePpm();
        CHECK(r >= 1 && r <= 2);
    }
    // At +600 s the same residual-rate may move the full 20 ppm.
    qpc += 600 * f; utc += 600000 + 80;   // +80 ms over 600 s = 133 ppm again
    Clock_OnSyncedNtpUtc(utc, qpc);
    {
        int32_t r = Clock_RatePpm();
        CHECK(r >= 15 && r <= 22);          // 2 + up to 20 (16 requested)
    }
}

// A sync shorter than the 30 s minimum interval must not update the rate
// (guards against a huge corr from a tiny denominator).
static void test_clock_short_interval_guard(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t f   = clock_test_qpc_freq();
    int64_t utc = make_utc_ms(2026, 5, 6, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(utc, qpc);

    // One long cycle establishes a measured rate.
    qpc += f * 3600;
    utc += 3600000 + 1000;              // +1 s error over 1 h
    Clock_OnSyncedNtpUtc(utc, qpc);
    int32_t established = Clock_RatePpm();
    CHECK(established > 0);

    // A 10 s cycle with a big error must leave the rate untouched.
    qpc += f * 10;
    utc += 10000 + 2000;
    Clock_OnSyncedNtpUtc(utc, qpc);
    CHECK_EQ_INT(Clock_RatePpm(), established);
}

// A normal suspend/resume where QPC tracked real time (utc and qpc advance
// together) must NOT false-trip INOP -- the projection still matches.
static void test_clock_resume_consistent(void) {
    clock_test_reset_appdata();
    Clock_Init();
    int64_t f   = clock_test_qpc_freq();
    int64_t utc = make_utc_ms(2026, 5, 7, 0, 0, 0);
    int64_t qpc = Clock_Qpc();
    Clock_OnPollCycle(TRUST_OK, utc, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);

    int64_t dt = 7200;                  // "sleep" 2 h, QPC counted through it
    qpc += f * dt;
    utc += dt * 1000;
    Clock_OnPollCycle(TRUST_OK, utc, qpc, 0);
    CHECK_EQ_INT(Clock_Trust(), TRUST_OK);          // no false INOP
    CHECK_EQ_INT(Clock_IsDisciplined(), 1);
}

// A kiss-o'-death reply removes the sender from the cycle draw: RATE
// (and unknown codes) for a 15-min cooldown, DENY/RSTR for the session.
static void test_ntp_kiss_of_death(void) {
    Ntp_TestClearKissOfDeath();
    int full = Ntp_TestEligibleCoreCount();
    CHECK(full >= 8);   // pool must cover 4 slots x 2 attempts

    Ntp_TestMarkKissOfDeath(0, "RATE");
    Ntp_TestMarkKissOfDeath(1, "DENY");
    Ntp_TestMarkKissOfDeath(2, "RSTR");
    CHECK_EQ_INT(Ntp_TestEligibleCoreCount(), full - 3);

    Ntp_TestClearKissOfDeath();
    CHECK_EQ_INT(Ntp_TestEligibleCoreCount(), full);
}

// Windows->IANA timezone map: the generated table must be sorted (the
// binary-search invariant), and every mapped IANA name must actually
// resolve in Lunar's embedded index -- the real point of the filter.
static void test_tz_winmap(void) {
    int n = TzWinmap_Count();
    CHECK(n > 100);

    // Sorted ascending by the Windows key (wcscmp order).
    for (int i = 1; i < n; i++) {
        CHECK(wcscmp(g_tz_winmap[i - 1].win, g_tz_winmap[i].win) < 0);
    }

    // Every mapped IANA name is present in the embedded tz index.
    for (int i = 0; i < n; i++) {
        CHECK(Tz_FindByName(g_tz_winmap[i].iana) != TZ_ID_INVALID);
    }

    // Spot-check well-known mappings (canonicalized where CLDR is legacy).
    const char *berlin = TzWinmap_IanaFromWindows(L"W. Europe Standard Time");
    CHECK(berlin && strcmp(berlin, "Europe/Berlin") == 0);
    const char *la = TzWinmap_IanaFromWindows(L"Pacific Standard Time");
    CHECK(la && strcmp(la, "America/Los_Angeles") == 0);
    const char *kolkata = TzWinmap_IanaFromWindows(L"India Standard Time");
    CHECK(kolkata && strcmp(kolkata, "Asia/Kolkata") == 0);   // not Calcutta
    CHECK(TzWinmap_IanaFromWindows(L"No Such Zone Key") == nullptr);

    // The machine's own zone must map to an embedded name.
    char iana[64];
    CHECK_EQ_INT(TzWinmap_CurrentIana(iana, sizeof iana), 1);
    CHECK(Tz_FindByName(iana) != TZ_ID_INVALID);
}

// ---------------------------------------------------------------------------
// Ntp_Concur -- the pure trust-verdict evaluator (NTS-anchored)
// ---------------------------------------------------------------------------
//
// Contract (from ntp.h): both NTS slots must succeed, both must be
// authenticated by enrolled pins, and they must come from different
// operator families before core concurrence can promote the cycle.

static NtpSourceResult MkSrc(int ok, int64_t utc, int64_t qpc,
                             const char *label) {
    NtpSourceResult r = {0};
    r.ok       = ok;
    r.offsetMs = 0;       // display-only, not used by Ntp_Concur
    r.ntpUtcMs = utc;
    r.qpcAtT4  = qpc;
    r.rttMs    = 10;
    r.label    = label;
    if (label && strcmp(label, "NTS1") == 0) {
        r.authMode = NTP_AUTH_ENROLLED_PIN;
        r.operatorFamily = "family-a";
    } else if (label && strcmp(label, "NTS2") == 0) {
        r.authMode = NTP_AUTH_ENROLLED_PIN;
        r.operatorFamily = "family-b";
    } else if (ok) {
        r.authMode = NTP_AUTH_PLAIN_SNTP;
    }
    return r;
}

static void test_ntp_concur(void) {
    // Clock_Init is required so Clock_QpcFreq() returns a valid
    // frequency for the projection code path. It is idempotent.
    Clock_Init();

    // Layout (from ntp.h): slots 0..NTP_CORE_COUNT-1 are core SNTP,
    // slots NTP_FIRST_NTS_SLOT..NTP_SOURCE_COUNT-1 are NTS. Verify
    // compile-time assumptions this test relies on so a future
    // re-tuning of the slot counts trips the assertion rather than
    // silently corrupting scenario indices.
    CHECK_EQ_INT(NTP_CORE_COUNT, 4);
    CHECK_EQ_INT(NTP_NTS_COUNT, 2);
    CHECK_EQ_INT(NTP_FIRST_NTS_SLOT, 4);
    CHECK_EQ_INT(NTP_SOURCE_COUNT, 6);

    NtpSourceResult s[NTP_SOURCE_COUNT];
    int64_t best = 0, qpc = 0, spread = 0;
    const int64_t Q = 1000;  // shared qpcAtT4 -- projection delta = 0

    // ---- Path A: both NTS succeed and mutually agree ----

    // 1) All six ok, all UTC identical -> OK, midpoint = 1000.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    // Measured anchorErr: pair spread 0/2 + worst NTS rtt 10/2 + 15 ms
    // root-dispersion floor = 20.
    CHECK_EQ_INT(spread, 20);
    CHECK_EQ_INT(best, 1000);
    CHECK_EQ_INT(qpc, Q);

    // 2) NTS pair brackets truth: 990 and 1010 -> midpoint 1000.
    // Cores at 1000 -> all four agree -> OK.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1,  990, Q, "NTS1");
    s[5] = MkSrc(1, 1010, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(best, 1000);        // midpoint of 990 and 1010

    // 3) Both NTS agree, 3 of 4 cores concur, 1 core far off -> OK.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1100, Q, "B");   // +100 ok
    s[2] = MkSrc(1,  900, Q, "C");   // -100 ok
    s[3] = MkSrc(1, 1500, Q, "D");   // +500 OUTLIER
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(best, 1000);
    // anchorErr = max(base 20, median dev of the CONCURRING cores
    // {0,100,100} = 100); the excluded outlier cannot inflate it.
    CHECK_EQ_INT(spread, 100);
    // The scheduler's convergence input is the CONCURRING cores' worst
    // deviation from the anchor (100 here) -- pure agreement, no RTT term,
    // and the excluded outlier (500) does not enter it either.
    CHECK_EQ_INT((int)Ntp_LastSpreadMs(), 100);

    // 4) Both NTS agree, only 2 of 4 cores concur -> INOP (<3).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1500, Q, "C");
    s[3] = MkSrc(1, 1500, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    best = qpc = -1;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
    CHECK_EQ_INT(best, 0); CHECK_EQ_INT(qpc, 0);
    CHECK_EQ_INT((int)Ntp_LastSpreadMs(), 500);   // worst core when the gate fails

    // 5) Both NTS agree, 3 cores OK, 1 core failed -> OK (3 of 4).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(0,    0, 0, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);

    // 6) Both NTS disagree by 201 ms -> INOP, spread reports NTS gap.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1201, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
    CHECK_EQ_INT(spread, 201);

    // 7) Both NTS exactly 200 ms apart (boundary) -> OK.
    s[4] = MkSrc(1,  900, Q, "NTS1");
    s[5] = MkSrc(1, 1100, Q, "NTS2");
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(best, 1000);

    // ---- Path B: NTS unavailable (< 2 ok) -> INOP, cluster witness ----
    // There is no intermediate tier any more: fewer than two authenticated
    // anchors is a hard INOP. A tight >=3 core cluster is reported ONLY as
    // a widen-only witness (Clock_OnCoreWitness input), never as trust.

    // 8) Only NTS1 ok, all 4 cores agree within 100ms -> INOP + cluster.
    int64_t clUtc = 0, clQpc = 0; int haveCl = 0;
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(0,    0, 0, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, &clUtc, &clQpc, &haveCl),
                 TRUST_INOP);
    CHECK_EQ_INT(haveCl, 1);
    CHECK_EQ_INT(clUtc, 1000);
    CHECK_EQ_INT(best, 0);            // no verdict consensus on INOP

    // 9) Only NTS2 ok, 3 of 4 cores agree within 100ms (1 outlier)
    // -> INOP + cluster from the agreeing three.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1500, Q, "D");     // one outlier
    s[4] = MkSrc(0,    0, 0, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    haveCl = 0;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, &clUtc, &clQpc, &haveCl),
                 TRUST_INOP);
    CHECK_EQ_INT(haveCl, 1);

    // 10) Only NTS1 ok, 3 cores ok within 100ms (1 failed)
    // -> INOP + cluster.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(0,    0, 0, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(0,    0, 0, "NTS2");
    haveCl = 0;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, &clUtc, &clQpc, &haveCl),
                 TRUST_INOP);
    CHECK_EQ_INT(haveCl, 1);

    // ---- Path C: no NTS at all ----

    // 11) Both NTS are same operator family -> INOP.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    s[5].operatorFamily = "family-a";
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);

    // 12) Both NTS fail, all 4 cores agree within 100ms -> INOP with the
    // cluster reported for the widen-only watchdog.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(0,    0, 0, "NTS1");
    s[5] = MkSrc(0,    0, 0, "NTS2");
    haveCl = 0;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, &clUtc, &clQpc, &haveCl),
                 TRUST_INOP);
    CHECK_EQ_INT(haveCl, 1);
    CHECK_EQ_INT(clUtc, 1000);

    // 13) Everything fails -> INOP.
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) s[i] = MkSrc(0, 0, 0, "x");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);

    // ---- NULL out-parameters ----

    // 14) NULL outs tolerated on both verdict paths.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, NULL, NULL, NULL, NULL, NULL, NULL), TRUST_OK);
    s[4] = MkSrc(0, 0, 0, "NTS1");
    s[5] = MkSrc(0, 0, 0, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, NULL, NULL, NULL, NULL, NULL, NULL), TRUST_INOP);

    // ---- Negative UTCs (pre-1970) ----

    // 15) Negative UTCs work on the full-OK path.
    s[0] = MkSrc(1, -100, Q, "A");
    s[1] = MkSrc(1, -100, Q, "B");
    s[2] = MkSrc(1, -100, Q, "C");
    s[3] = MkSrc(1, -100, Q, "D");
    s[4] = MkSrc(1, -100, Q, "NTS1");
    s[5] = MkSrc(1, -100, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(best, -100);

    // ---- QPC projection across staggered captures ----

    int64_t freq = Clock_QpcFreq();
    CHECK(freq > 0);
    int64_t tick100ms = freq / 10;

    // 16) Cores captured at different qpc moments with UTC staggered
    // to match perfectly -- projected onto the NTS midpoint they
    // should all concur with zero spread.
    s[0] = MkSrc(1, 1000 - 100, Q - tick100ms,     "A");
    s[1] = MkSrc(1, 1000 + 100, Q + tick100ms,     "B");
    s[2] = MkSrc(1, 1000 + 200, Q + 2 * tick100ms, "C");
    s[3] = MkSrc(1, 1000 - 200, Q - 2 * tick100ms, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(spread, 20);   // rtt/2 + disp floor; projections agree
    CHECK_EQ_INT(best, 1000);

    // 17) Hold core UTC constant while qpcAtT4 drifts past the NTS
    // pair by >= 300 ms each. Projected deltas diverge -> INOP
    // because fewer than 3 cores remain within 200 ms.
    int64_t tick300ms = (freq * 3) / 10;
    s[0] = MkSrc(1, 1000, Q + tick300ms,     "A");  // +300 proj
    s[1] = MkSrc(1, 1000, Q + 2 * tick300ms, "B");  // +600 proj
    s[2] = MkSrc(1, 1000, Q + 3 * tick300ms, "C");  // +900 proj
    s[3] = MkSrc(1, 1000, Q + 4 * tick300ms, "D");  // +1200 proj
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
    CHECK(spread >= 1199 && spread <= 1201);

    // 18) Two NTS captured at different qpc moments whose UTCs
    // stagger to match -- after projection they must agree. Put
    // NTS2 100 ms in the future (both qpc and utc) so projection
    // onto NTS1's qpc collapses the delta to ~zero.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000,       Q,              "NTS1");
    s[5] = MkSrc(1, 1000 + 100, Q + tick100ms,  "NTS2");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    // Midpoint projection: NTS2 projected onto NTS1's qpc = 1000,
    // so midpoint = 1000.
    CHECK_EQ_INT(best, 1000);
}

// ---------------------------------------------------------------------------
// Ntp_Concur -- ROTATED_PIN gating (corroborated pin rotation)
// ---------------------------------------------------------------------------
//
// A ROTATED_PIN slot (CA-validated leaf that matched no stored pin
// outside the renewal window) may count toward the 2-NTS gate ONLY when
// the other slot is a continuous ENROLLED_PIN from a different operator
// family. Two rotated slots have no continuous corroborator and must
// hard-fail: that is exactly the shape of a CA-level MITM against both
// anchors, so the cycle hard-fails to INOP.
static void test_ntp_concur_rotated(void) {
    Clock_Init();

    NtpSourceResult s[NTP_SOURCE_COUNT];
    int64_t best = 0, qpc = 0, spread = 0;
    const int64_t Q = 1000;

    // R1) Enrolled(family-a) + rotated(family-b), agreeing, all cores
    // concur -> full TRUST_OK (rotation rides on the continuous peer).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    s[5].authMode = NTP_AUTH_ROTATED_PIN;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);
    CHECK_EQ_INT(best, 1000);
    CHECK_EQ_INT(qpc, Q);

    // R2) Same but the rotated slot comes first -> order-independent.
    s[4].authMode = NTP_AUTH_ROTATED_PIN;
    s[5].authMode = NTP_AUTH_ENROLLED_PIN;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_OK);

    // R3) BOTH slots rotated (diverse families, perfect agreement,
    // full core concurrence) -> INOP. Never OK, no weaker credit.
    s[4].authMode = NTP_AUTH_ROTATED_PIN;
    s[5].authMode = NTP_AUTH_ROTATED_PIN;
    best = qpc = -1;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
    CHECK_EQ_INT(best, 0);
    CHECK_EQ_INT(qpc, 0);

    // R4) Rotated + enrolled but SAME operator family -> INOP.
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    s[5].authMode = NTP_AUTH_ROTATED_PIN;
    s[5].operatorFamily = "family-a";
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);

    // R5) Rotated + enrolled, diverse families, but disagreeing by
    // 201 ms -> INOP (mutual-agreement gate unchanged).
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1201, Q, "NTS2");
    s[5].authMode = NTP_AUTH_ROTATED_PIN;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
    CHECK_EQ_INT(spread, 201);

    // R6) A lone rotated slot (other NTS failed) cannot anchor: the cycle
    // is INOP like any single-NTS cycle; the agreeing cores surface only
    // as the widen-only cluster witness.
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[4].authMode = NTP_AUTH_ROTATED_PIN;
    s[5] = MkSrc(0, 0, 0, "NTS2");
    {
        int64_t r6ClUtc = 0, r6ClQpc = 0; int r6Have = 0;
        CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread,
                                &r6ClUtc, &r6ClQpc, &r6Have), TRUST_INOP);
        CHECK_EQ_INT(r6Have, 1);
        CHECK_EQ_INT(r6ClUtc, 1000);
    }

    // R7) Enrolled + rotated agreeing, but only 2 of 4 cores concur ->
    // INOP (core super-majority still required with a rotation in play).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1500, Q, "C");
    s[3] = MkSrc(1, 1500, Q, "D");
    s[4] = MkSrc(1, 1000, Q, "NTS1");
    s[5] = MkSrc(1, 1000, Q, "NTS2");
    s[5].authMode = NTP_AUTH_ROTATED_PIN;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread, NULL, NULL, NULL), TRUST_INOP);
}

// ---------------------------------------------------------------------------
// AES-SIV-CMAC-256 -- RFC 5297 known-answer tests + round-trip checks
// ---------------------------------------------------------------------------
//
// SIV is the AEAD NTS uses to authenticate SNTP packets. We need this
// to be exactly right: a silent bug here would either break authentic
// cookie/message exchange (failing closed, visible) or, worse, accept
// a forged authenticator (failing open, invisible). The RFC ships a
// pair of worked examples in Appendix A; we verify both, then add a
// few round-trip fuzzes that exercise edge cases (empty AD, empty PT,
// PT crossing the 16-byte S2V boundary, decrypt-tamper detection).

#include "../src/siv.h"

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s) {
        if (*s == ' ' || *s == '\n') { s++; continue; }
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0 || n >= cap) return 0;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}
static int bufs_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void test_siv_rfc5297_appendix_a1(void) {
    // RFC 5297 Appendix A.1: deterministic authenticated encryption
    // with a single associated-data element.
    //
    //   Key:        fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0
    //               f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff
    //   AD:         101112131415161718191a1b1c1d1e1f2021222324252627
    //   Plaintext:  112233445566778899aabbccddee
    //   V (tag):    85632d07c6e8f37f950acd320a2ecc93
    //   Cipher:     40c02b9690c4dc04daef7f6afe5c
    uint8_t key[32], ad[32], pt[32], want_tag[16], want_ct[32];
    size_t key_n = unhex("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
                         "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", key, sizeof key);
    size_t ad_n  = unhex("101112131415161718191a1b1c1d1e1f"
                         "2021222324252627", ad, sizeof ad);
    size_t pt_n  = unhex("112233445566778899aabbccddee", pt, sizeof pt);
    size_t tag_n = unhex("85632d07c6e8f37f950acd320a2ecc93",
                         want_tag, sizeof want_tag);
    size_t ct_n  = unhex("40c02b9690c4dc04daef7f6afe5c",
                         want_ct, sizeof want_ct);
    CHECK_EQ_INT(key_n, 32);
    CHECK_EQ_INT(ad_n, 24);
    CHECK_EQ_INT(pt_n, 14);
    CHECK_EQ_INT(tag_n, 16);
    CHECK_EQ_INT(ct_n, 14);

    SivSlice ad_vec = { ad, ad_n };
    uint8_t out[64];
    CHECK_EQ_INT(Siv_Encrypt(key, &ad_vec, 1, pt, pt_n, out), 0);
    CHECK(bufs_eq(out,        want_tag, 16));    // SIV matches RFC
    CHECK(bufs_eq(out + 16,   want_ct,  pt_n));  // CTR body matches RFC

    // Round-trip: the same AD + key must recover the plaintext exactly.
    uint8_t pt_back[32];
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, out, 16 + pt_n, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, pt_n));
}

static void test_siv_rfc5297_appendix_a2(void) {
    // RFC 5297 Appendix A.2: multiple AD elements, longer plaintext
    // that spans three 16-byte S2V blocks (so the "xorend" path runs).
    //
    //   Key:        7f7e7d7c7b7a79787776757473727170
    //               404142434445464748494a4b4c4d4e4f
    //   AD1:        00112233445566778899aabbccddeeff
    //               deaddadadeaddadaffeeddccbbaa9988
    //               7766554433221100
    //   AD2:        102030405060708090a0
    //   Nonce:      09f911029d74e35bd84156c5635688c0
    //   Plaintext:  7468697320697320736f6d6520706c61
    //               696e7465787420746f20656e63727970
    //               74207573696e67205349562d414553
    //   V (tag):    7bdb6e3b432667eb06f4d14bff2fbd0f
    //   Cipher:     cb900f2fddbe404326601965c889bf17
    //               dba77ceb094fa663b7a3f748ba8af829
    //               ea64ad544a272e9c485b62a3fd5c0d
    uint8_t key[32], ad1[48], ad2[16], nonce[16], pt[64], want_tag[16], want_ct[64];
    size_t key_n = unhex("7f7e7d7c7b7a79787776757473727170"
                         "404142434445464748494a4b4c4d4e4f", key, sizeof key);
    size_t ad1_n = unhex("00112233445566778899aabbccddeeff"
                         "deaddadadeaddadaffeeddccbbaa9988"
                         "7766554433221100", ad1, sizeof ad1);
    size_t ad2_n = unhex("102030405060708090a0", ad2, sizeof ad2);
    size_t nc_n  = unhex("09f911029d74e35bd84156c5635688c0", nonce, sizeof nonce);
    size_t pt_n  = unhex("7468697320697320736f6d6520706c61"
                         "696e7465787420746f20656e63727970"
                         "74207573696e67205349562d414553", pt, sizeof pt);
    size_t tag_n = unhex("7bdb6e3b432667eb06f4d14bff2fbd0f", want_tag, sizeof want_tag);
    size_t ct_n  = unhex("cb900f2fddbe404326601965c889bf17"
                         "dba77ceb094fa663b7a3f748ba8af829"
                         "ea64ad544a272e9c485b62a3fd5c0d", want_ct, sizeof want_ct);
    CHECK_EQ_INT(key_n, 32);
    CHECK_EQ_INT(ad1_n, 40);
    CHECK_EQ_INT(ad2_n, 10);
    CHECK_EQ_INT(nc_n, 16);
    CHECK_EQ_INT(pt_n, 47);
    CHECK_EQ_INT(tag_n, 16);
    CHECK_EQ_INT(ct_n, 47);

    // Per RFC 5297 §2.6: the nonce is the LAST AD element.
    SivSlice ad_vec[3] = {
        { ad1,   ad1_n },
        { ad2,   ad2_n },
        { nonce, nc_n  },
    };
    uint8_t out[128];
    CHECK_EQ_INT(Siv_Encrypt(key, ad_vec, 3, pt, pt_n, out), 0);
    CHECK(bufs_eq(out,      want_tag, 16));
    CHECK(bufs_eq(out + 16, want_ct,  pt_n));

    uint8_t pt_back[64];
    CHECK_EQ_INT(Siv_Decrypt(key, ad_vec, 3, out, 16 + pt_n, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, pt_n));
}

static void test_siv_edge_cases(void) {
    // Common NTS shape: one AD element (the SNTP header), zero-length
    // plaintext (empty encrypted-extensions field). Must still produce
    // a 16-byte authenticator and round-trip cleanly.
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;

    uint8_t ad[48];
    for (int i = 0; i < 48; i++) ad[i] = (uint8_t)(0xA0 + i);
    SivSlice ad_vec = { ad, sizeof ad };

    uint8_t tag[16];
    CHECK_EQ_INT(Siv_Encrypt(key, &ad_vec, 1, NULL, 0, tag), 0);
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, tag, 16, NULL), 0);

    // Flip one AD byte -> authentication MUST fail and the (empty)
    // plaintext buffer MUST NOT be written past length 0.
    ad[7] ^= 0x01;
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, tag, 16, NULL), -1);
    ad[7] ^= 0x01;

    // Zero AD, non-empty plaintext -- exercises the n==0 S2V path.
    uint8_t pt[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                       11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    uint8_t out2[64];
    CHECK_EQ_INT(Siv_Encrypt(key, NULL, 0, pt, sizeof pt, out2), 0);
    uint8_t pt_back[32];
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 16 + sizeof pt, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, sizeof pt));

    // Tamper one ciphertext byte -> auth must fail, plaintext wiped.
    out2[20] ^= 0x80;
    for (size_t i = 0; i < sizeof pt; i++) pt_back[i] = 0xCC;
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 16 + sizeof pt, pt_back), -1);
    for (size_t i = 0; i < sizeof pt; i++) CHECK_EQ_INT(pt_back[i], 0);

    // Short input (< 16 bytes) rejected outright without reading key/AD.
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 10, pt_back), -1);
}

// ---------------------------------------------------------------------------
// NTS-KE record codec -- RFC 8915 §4 serialise/parse on hand-crafted bytes
// ---------------------------------------------------------------------------

#include "../src/nts_ke.h"

static void test_ntske_client_request(void) {
    // The fixed client request must be exactly 16 bytes:
    //   Record 1 (NextProtocol, critical, body len 2, body [0x0000]) = 6
    //   Record 2 (AEAD,         critical, body len 2, body [0x000F]) = 6
    //   Record 3 (EndOfMessage, critical, body len 0)                = 4
    // All critical bits set.
    uint8_t buf[64] = { 0xaa };
    size_t n = NtsKe_BuildClientRequest(buf, sizeof buf);
    CHECK_EQ_INT(n, 16);

    // Record 1: type 0x8001, len 2, body 0x0000
    CHECK_EQ_INT(buf[0], 0x80);
    CHECK_EQ_INT(buf[1], 0x01);
    CHECK_EQ_INT(buf[2], 0x00);
    CHECK_EQ_INT(buf[3], 0x02);
    CHECK_EQ_INT(buf[4], 0x00);
    CHECK_EQ_INT(buf[5], 0x00);
    // Record 2: type 0x8004, len 2, body 0x000F
    CHECK_EQ_INT(buf[6],  0x80);
    CHECK_EQ_INT(buf[7],  0x04);
    CHECK_EQ_INT(buf[8],  0x00);
    CHECK_EQ_INT(buf[9],  0x02);
    CHECK_EQ_INT(buf[10], 0x00);
    CHECK_EQ_INT(buf[11], 0x0f);
    // Record 3: type 0x8000, len 0
    CHECK_EQ_INT(buf[12], 0x80);
    CHECK_EQ_INT(buf[13], 0x00);
    CHECK_EQ_INT(buf[14], 0x00);
    CHECK_EQ_INT(buf[15], 0x00);

    // Small output buffer must refuse cleanly.
    CHECK_EQ_INT(NtsKe_BuildClientRequest(buf, 10), 0);
    CHECK_EQ_INT(NtsKe_BuildClientRequest(NULL, 64), 0);
}

// Small helper: append a record to a mutable buffer.
static size_t append_rec(uint8_t *buf, size_t pos,
                         uint16_t type_crit, const uint8_t *body, uint16_t blen) {
    buf[pos++] = (uint8_t)(type_crit >> 8);
    buf[pos++] = (uint8_t)(type_crit & 0xff);
    buf[pos++] = (uint8_t)(blen >> 8);
    buf[pos++] = (uint8_t)(blen & 0xff);
    if (blen && body) { memcpy(buf + pos, body, blen); pos += blen; }
    return pos;
}

static void test_ntske_parse_valid(void) {
    // Build a realistic server response:
    //   NextProtocol=[NTPv4] crit
    //   AEAD=[SIV-CMAC-256] crit
    //   3x NewCookie (16 bytes each, non-critical)
    //   NTPv4Port=4123 (non-critical)
    //   NTPv4Server="ntp.example.org" (non-critical)
    //   EndOfMessage crit
    uint8_t buf[512];
    size_t  pos = 0;

    uint8_t np_body[2]  = { 0x00, 0x00 };
    uint8_t aead_body[2]= { 0x00, 0x0f };
    uint8_t cookie1[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t cookie2[16] = { 0xaa, 0xbb, 0xcc };
    uint8_t cookie3[16] = { 0xff };
    uint8_t port_body[2]= { 0x10, 0x1b };     // 4123
    const char *host    = "ntp.example.org";

    pos = append_rec(buf, pos, 0x8001, np_body, 2);
    pos = append_rec(buf, pos, 0x8004, aead_body, 2);
    pos = append_rec(buf, pos, 0x0005, cookie1, 16);
    pos = append_rec(buf, pos, 0x0005, cookie2, 16);
    pos = append_rec(buf, pos, 0x0005, cookie3, 16);
    pos = append_rec(buf, pos, 0x0007, port_body, 2);
    pos = append_rec(buf, pos, 0x0006, (const uint8_t *)host, (uint16_t)strlen(host));
    pos = append_rec(buf, pos, 0x8000, NULL, 0);

    NtsKeResponse r;
    CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
    CHECK(r.ok);
    CHECK(r.proto_ok);
    CHECK(r.aead_ok);
    CHECK_EQ_INT(r.cookie_count, 3);
    CHECK_EQ_INT(r.cookie_len[0], 16);
    CHECK_EQ_INT(r.cookie_len[1], 16);
    CHECK_EQ_INT(r.cookie_len[2], 16);
    CHECK(bufs_eq(r.cookies[0], cookie1, 16));
    CHECK(bufs_eq(r.cookies[1], cookie2, 16));
    CHECK(bufs_eq(r.cookies[2], cookie3, 16));
    CHECK_EQ_INT(r.ntp_port, 4123);
    CHECK_EQ_STR(r.ntp_host, "ntp.example.org");
    CHECK_EQ_INT(r.error_code, 0);
}

static void test_ntske_parse_errors(void) {
    NtsKeResponse r;

    // 1) Missing End-of-Message: parser must reject.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
        CHECK_EQ_INT(r.ok, 0);
    }

    // 2) Server sends Error record -> parse fails, code captured.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t err[2] = { 0x00, 0x02 };   // code 2 (Internal Server Error)
        pos = append_rec(buf, pos, 0x8002, err, 2);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
        CHECK_EQ_INT(r.ok, 0);
        CHECK_EQ_INT(r.error_code, 2);
    }

    // 3) Missing AEAD agreement -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 4) No cookies -> fail (server must supply at least one).
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 5) Unknown record with critical bit -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8042, NULL, 0);   // unknown critical
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 6) Unknown record with critical bit CLEAR -> ignored, success.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x0042, NULL, 0);   // unknown non-critical
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
        CHECK_EQ_INT(r.cookie_count, 1);
    }

    // 7) Truncated body length -> fail.
    {
        uint8_t buf[6] = { 0x80, 0x01, 0x00, 0x10, 0x00, 0x00 };  // claims 16-byte body
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, sizeof buf, &r), 0);
    }

    // 8) Trailing bytes after End-of-Message -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        buf[pos++] = 0x55;       // extra byte
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 9) NTPv4-server record with non-ASCII bytes -> host left empty but
    // (non-critical) overall parse still succeeds.
    {
        uint8_t buf[64]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        uint8_t bad_host[] = { 'n', 't', 'p', 0x01, 'x' };
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x0006, bad_host, sizeof bad_host);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
        CHECK_EQ_INT(r.ntp_host[0], 0);
    }
}

// ---------------------------------------------------------------------------
// NTS-SNTP extension field codec -- RFC 8915 §5 round-trip
// ---------------------------------------------------------------------------

#include "../src/nts_ef.h"

// Helper: craft a server reply that mirrors RFC 8915's server-side AEAD.
// Packet = 48-byte header
//          + Unique Identifier (echoed)
//          + Authenticator (s2c_key, nonce, AD=preceding bytes, PT=N new cookies).
static size_t build_server_reply(uint8_t *out, size_t cap,
                                 const uint8_t hdr[48],
                                 const uint8_t *echo_uid,
                                 const uint8_t nonce[16],
                                 const uint8_t s2c_key[32],
                                 const uint8_t *new_cookies,
                                 size_t new_cookie_len,
                                 size_t n_new_cookies) {
    size_t pos = 0;
    memcpy(out, hdr, 48);
    pos = 48;

    // Unique Identifier extension
    out[pos++] = 0x01; out[pos++] = 0x04;
    out[pos++] = 0x00; out[pos++] = 36;          // 4 + 32
    memcpy(out + pos, echo_uid, 32);
    pos += 32;

    // Build plaintext = concatenated NTS Cookie extensions
    uint8_t pt[512] = {0};
    size_t  pt_len = 0;
    for (size_t i = 0; i < n_new_cookies; i++) {
        size_t total = (4 + new_cookie_len + 3) & ~(size_t)3u;
        if (total < 16) total = 16;
        pt[pt_len++] = 0x02; pt[pt_len++] = 0x04;
        pt[pt_len++] = (uint8_t)(total >> 8);
        pt[pt_len++] = (uint8_t)(total & 0xff);
        memcpy(pt + pt_len, new_cookies + i * new_cookie_len, new_cookie_len);
        pt_len += new_cookie_len;
        while (pt_len % 4) pt[pt_len++] = 0;
    }

    // Authenticator header + nonce-len + ct-len
    size_t auth_pos = pos;
    size_t ct_len = pt_len + 16;                 // tag + enc(plaintext)
    size_t auth_total = (4 + 4 + 16 + ct_len + 3) & ~(size_t)3u;

    out[pos++] = 0x04; out[pos++] = 0x04;
    out[pos++] = (uint8_t)(auth_total >> 8);
    out[pos++] = (uint8_t)(auth_total & 0xff);
    out[pos++] = 0x00; out[pos++] = 16;          // nonce len
    out[pos++] = (uint8_t)(ct_len >> 8);
    out[pos++] = (uint8_t)(ct_len & 0xff);
    memcpy(out + pos, nonce, 16);
    pos += 16;

    // SIV-encrypt the plaintext with AD = [in[0..auth_pos], nonce]
    SivSlice ad[2] = { { out, auth_pos }, { nonce, 16 } };
    uint8_t siv_out[1024];
    if (Siv_Encrypt(s2c_key, ad, 2, pt, pt_len, siv_out) != 0) return 0;
    memcpy(out + pos, siv_out, ct_len);
    pos += ct_len;

    while (pos - auth_pos < auth_total) out[pos++] = 0;

    (void)cap;
    return pos;
}

static void test_nts_ef_roundtrip(void) {
    // Keys: 32 bytes C2S and 32 bytes S2C (arbitrary; any random).
    uint8_t c2s_key[32], s2c_key[32];
    for (int i = 0; i < 32; i++) { c2s_key[i] = (uint8_t)(0x10 + i); s2c_key[i] = (uint8_t)(0x80 + i); }

    uint8_t ntp_hdr[48] = {0};
    ntp_hdr[0] = 0x23;                              // LI=0, VN=4, Mode=3

    uint8_t uid[32];
    for (int i = 0; i < 32; i++) uid[i] = (uint8_t)(0xAB ^ i);
    uint8_t nonce_c[16];
    for (int i = 0; i < 16; i++) nonce_c[i] = (uint8_t)(0x55 + i);

    uint8_t cookie[64];
    for (int i = 0; i < 64; i++) cookie[i] = (uint8_t)(0xC0 + i);

    uint8_t req[512];
    size_t  req_len = 0;
    int rc = NtsEf_BuildRequest(ntp_hdr, uid, nonce_c,
                                cookie, sizeof cookie,
                                /*n_placeholder=*/3,
                                c2s_key,
                                req, sizeof req, &req_len);
    CHECK_EQ_INT(rc, 0);

    // Minimum expected size: 48 + 36 (UID) + 68 (cookie) + 3*68 (placeholders)
    //                       + 40 (auth) = 396.
    CHECK_EQ_INT(req_len, 48 + 36 + 68 + 3 * 68 + 40);

    // Now verify self-consistency by running the parser over a
    // server-shape reply we craft using the SAME SIV primitive.
    // The server echoes UID, uses s2c_key, and returns 4 new cookies
    // (one for the spent cookie + 3 placeholder tops-up).
    uint8_t new_cookies[4 * 64];
    for (int i = 0; i < 4 * 64; i++) new_cookies[i] = (uint8_t)(0xF0 + (i & 0x0F));
    uint8_t nonce_s[16];
    for (int i = 0; i < 16; i++) nonce_s[i] = (uint8_t)(0xAA + i);

    uint8_t hdr_reply[48] = {0};
    hdr_reply[0] = 0x24;      // server mode
    hdr_reply[1] = 1;         // stratum
    // Transmit Timestamp non-zero so a real NTP validator wouldn't
    // reject (not that our codec reads it -- it's just AD bytes).
    hdr_reply[40] = 0x12; hdr_reply[41] = 0x34;

    uint8_t reply[512];
    size_t  reply_len = build_server_reply(reply, sizeof reply,
                                           hdr_reply, uid, nonce_s, s2c_key,
                                           new_cookies, 64, 4);
    CHECK(reply_len > 0);

    uint8_t got_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  got_lens[NTSKE_MAX_COOKIES];
    size_t  got_n = 99;

    rc = NtsEf_ParseResponse(reply, reply_len, uid, s2c_key,
                             got_cookies, got_lens, &got_n);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(got_n, 4);
    for (size_t i = 0; i < 4; i++) {
        CHECK_EQ_INT(got_lens[i], 64);
        CHECK(bufs_eq(got_cookies[i], new_cookies + i * 64, 64));
    }
}

static void test_nts_ef_tamper(void) {
    uint8_t s2c_key[32];
    for (int i = 0; i < 32; i++) s2c_key[i] = (uint8_t)(0xE0 + i);
    uint8_t uid[32], nonce_s[16];
    for (int i = 0; i < 32; i++) uid[i]     = (uint8_t)(0x10 + i);
    for (int i = 0; i < 16; i++) nonce_s[i] = (uint8_t)(0x20 + i);

    uint8_t new_c[64];
    for (int i = 0; i < 64; i++) new_c[i] = (uint8_t)(0x88 + i);

    uint8_t hdr_s[48] = {0}; hdr_s[0] = 0x24; hdr_s[1] = 1;

    uint8_t reply[512];
    size_t  reply_len = build_server_reply(reply, sizeof reply,
                                           hdr_s, uid, nonce_s, s2c_key,
                                           new_c, 64, 1);
    CHECK(reply_len > 0);

    uint8_t gc[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  gl[NTSKE_MAX_COOKIES];
    size_t  gn;

    // 1) Clean parse succeeds.
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), 0);

    // 2) Flip one byte of the UID in the reply -> mismatch, fail.
    reply[52] ^= 0x01;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[52] ^= 0x01;

    // 3) Flip one byte of the NTP header -> AD mismatch, SIV fails.
    reply[10] ^= 0x80;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[10] ^= 0x80;

    // 4) Flip one byte of the ciphertext -> SIV auth fails.
    // Find the Ciphertext field: ends at reply_len.
    reply[reply_len - 4] ^= 0x40;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[reply_len - 4] ^= 0x40;

    // 5) Wrong S2C key -> SIV auth fails.
    uint8_t bad_key[32];
    memcpy(bad_key, s2c_key, 32); bad_key[0] ^= 1;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, bad_key, gc, gl, &gn), -1);

    // 6) Truncate to below 48 bytes -> reject.
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, 47, uid, s2c_key, gc, gl, &gn), -1);

    // 7) Parse with a different sent UID -> reject.
    uint8_t wrong_uid[32];
    memcpy(wrong_uid, uid, 32); wrong_uid[5] ^= 0x02;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, wrong_uid, s2c_key, gc, gl, &gn), -1);
}

// ---------------------------------------------------------------------------
// Rolling log buffer
// ---------------------------------------------------------------------------

#include "../src/logbuf.h"

static void test_logbuf_basic(void) {
    Log_Reset();   // isolate from prior tests' log volume
    Log_Append("alpha=%d", 1);
    Log_Append("beta %s", "two");

    char out[2048];
    size_t w = Log_Snapshot(out, sizeof out);
    CHECK(w > 0);
    CHECK(strstr(out, "alpha=1") != NULL);
    CHECK(strstr(out, "beta two") != NULL);
    // alpha appears before beta (ring stores in append order).
    CHECK(strstr(out, "alpha=1") < strstr(out, "beta two"));
    // Each line ends in CRLF.
    CHECK(strstr(out, "alpha=1\r\n") != NULL);
}

static void test_logbuf_truncation(void) {
    // Snapshot into a tiny buffer must NUL-terminate at cap-1.
    char small[16];
    memset(small, 0x55, sizeof small);
    Log_Append("some-fresh-line");
    Log_Snapshot(small, sizeof small);
    CHECK_EQ_INT(small[sizeof small - 1], 0);
}

static void test_logbuf_collect_since(void) {
    Log_Reset();
    Log_Append("first");
    Log_Append("second");
    Log_Append("third");

    static LogRawEntry all[8];
    size_t n = Log_CollectSince(0, all, 8);
    CHECK_EQ_INT((int)n, 3);
    CHECK(strcmp(all[0].msg, "first") == 0);
    CHECK(strcmp(all[2].msg, "third") == 0);
    // seq strictly increases in append order and survives Log_Reset
    // (process-lifetime monotonic).
    CHECK(all[1].seq == all[0].seq + 1);
    CHECK(all[2].seq == all[1].seq + 1);

    // Incremental read: only entries after sinceSeq come back.
    static LogRawEntry tail[8];
    size_t m = Log_CollectSince(all[0].seq, tail, 8);
    CHECK_EQ_INT((int)m, 2);
    CHECK(strcmp(tail[0].msg, "second") == 0);
    CHECK(strcmp(tail[1].msg, "third") == 0);

    // Nothing new: empty result, not a repeat.
    CHECK_EQ_INT((int)Log_CollectSince(all[2].seq, tail, 8), 0);

    // cap limits the copy but keeps oldest-first order.
    static LogRawEntry two[2];
    CHECK_EQ_INT((int)Log_CollectSince(0, two, 2), 2);
    CHECK(strcmp(two[0].msg, "first") == 0);

    // A new append after Reset continues the seq sequence.
    uint64_t lastSeq = all[2].seq;
    Log_Reset();
    Log_Append("fourth");
    CHECK_EQ_INT((int)Log_CollectSince(0, tail, 8), 1);
    CHECK(strcmp(tail[0].msg, "fourth") == 0);
    CHECK(tail[0].seq == lastSeq + 1);
}

// ---------------------------------------------------------------------------
// DoH resolver runtime policy: out-of-window rotation corroboration and
// failure back-off (pure decision cores, driven by caller-supplied ticks).
// ---------------------------------------------------------------------------
extern int  Dns_TestObserveRotation(size_t resolverIdx, const uint8_t spki[32],
                                    uint64_t nowTick);
extern int  Dns_TestNoteFailure(size_t resolverIdx, uint64_t nowTick);
extern int  Dns_TestBackedOff(size_t resolverIdx, uint64_t nowTick);
extern void Dns_TestResetResolverState(void);

static void test_doh_rotation_corroboration(void) {
    Dns_TestResetResolverState();
    uint8_t k1[32], k2[32], k3[32], k4[32];
    memset(k1, 0x11, sizeof k1);
    memset(k2, 0x22, sizeof k2);
    memset(k3, 0x33, sizeof k3);
    memset(k4, 0x44, sizeof k4);
    const uint64_t MIN = 60 * 1000;
    // First sighting of a CA-valid, unpinned key: a new candidate.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k1, 1000), 0 /* NEW */);
    // Seen again too soon: pending, never promoted.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k1, 1000 + 5 * MIN), 1 /* PENDING */);
    // A second key takes the second slot (an anycast provider may
    // alternate between two POP keys); k1 stays a live candidate.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k2, 1000 + 6 * MIN), 0 /* NEW */);
    // The same key again >= 10 min after ITS first sighting promotes.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k2, 1000 + 16 * MIN), 2 /* PROMOTE */);
    // Consumed: the next sighting of k2 starts a fresh candidate.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k2, 1000 + 17 * MIN), 0);
    // k1 (first seen at t=1000) is still live and promotes now.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k1, 1000 + 18 * MIN), 2);
    // Eviction: with both slots live (k3 new, k2 since 17 min) a further
    // key evicts the OLDEST, so a flapping interposer never accumulates
    // age: k2 comes back as NEW, not as a 23-minute-old candidate.
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k3, 1000 + 19 * MIN), 0);
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k4, 1000 + 20 * MIN), 0);   // evicts k2
    CHECK_EQ_INT(Dns_TestObserveRotation(0, k2, 1000 + 40 * MIN), 0);
    // Resolvers keep independent candidates.
    CHECK_EQ_INT(Dns_TestObserveRotation(1, k1, 5000), 0);
    CHECK_EQ_INT(Dns_TestObserveRotation(1, k1, 5000 + 11 * 60 * 1000), 2);
}

static void test_doh_failure_backoff(void) {
    Dns_TestResetResolverState();
    uint64_t t = 100000;
    CHECK_EQ_INT(Dns_TestBackedOff(2, t), 0);
    CHECK_EQ_INT(Dns_TestNoteFailure(2, t), 0);          // 1
    CHECK_EQ_INT(Dns_TestNoteFailure(2, t), 0);          // 2
    CHECK_EQ_INT(Dns_TestNoteFailure(2, t), 1);          // 3 -> back off
    CHECK_EQ_INT(Dns_TestBackedOff(2, t + 1000), 1);
    CHECK_EQ_INT(Dns_TestBackedOff(2, t + 59 * 60 * 1000), 1);
    CHECK_EQ_INT(Dns_TestBackedOff(2, t + 61 * 60 * 1000), 0);   // expired
    CHECK_EQ_INT(Dns_TestBackedOff(0, t), 0);            // others untouched
}

// The NTS picker learns each provider's RTT and draws near providers
// first, so a transatlantic anchor never sits in the pair while a near
// member of the same operator family answers.
static void test_nts_picker_prefers_near_providers(void) {
    Nts_TestResetPicker();
    size_t n = 0;
    const NtsProvider *pool = Nts_Pool(&n);
    CHECK(n >= 4);
    // EMA: first report seeds, later ones average 3:1.
    Nts_ReportRtt(&pool[0], 40);
    CHECK_EQ_INT((int)Nts_TestProviderRtt(0), 40);
    Nts_ReportRtt(&pool[0], 80);
    CHECK_EQ_INT((int)Nts_TestProviderRtt(0), 50);
    // Mark every provider whose host is on another continent as far
    // (system76 ohio/virginia/oregon), everything else near.
    for (size_t i = 0; i < n; i++) {
        const char *h = pool[i].host;   // `far` is a windef.h macro
        int isFar = strstr(h, "ohio.") == h || strstr(h, "virginia.") == h ||
                    strstr(h, "oregon.") == h;
        Nts_ReportRtt(&pool[i], isFar ? 300 : 40);
    }
    for (int round = 0; round < 8; round++) {
        Nts_ForceRepick();
        const NtsProvider *out[2] = { NULL, NULL };
        CHECK_EQ_INT((int)Nts_PickProviders(out, 2), 2);
        for (int k = 0; k < 2; k++) {
            CHECK(out[k] != NULL);
            if (!out[k]) continue;
            CHECK((int)Nts_TestProviderRtt((size_t)(out[k] - pool)) <= 100);
        }
        // operator diversity still holds within the near tier
        if (out[0] && out[1]) {
            CHECK(strcmp(out[0]->operator_family, out[1]->operator_family) != 0);
        }
    }
    Nts_TestResetPicker();
}

// ---------------------------------------------------------------------------
// HTTP/2 / HPACK (h2.c): the RFC 7541 Appendix C vectors, the canonical-code
// invariants of the Huffman table, and scripted exchanges through
// H2_PostOnce with a fake transport.
// ---------------------------------------------------------------------------
#include "../src/h2.h"

// Header fields as delivered by the decoder, joined "name: value\n".
typedef struct { char text[2048]; size_t len; int count; } HdrLog;
static void hdrlog_add(void *ctx, const uint8_t *n, size_t nl,
                       const uint8_t *v, size_t vl) {
    HdrLog *h = (HdrLog *)ctx;
    if (h->len + nl + vl + 3 >= sizeof h->text) return;
    memcpy(h->text + h->len, n, nl); h->len += nl;
    h->text[h->len++] = ':'; h->text[h->len++] = ' ';
    memcpy(h->text + h->len, v, vl); h->len += vl;
    h->text[h->len++] = '\n'; h->text[h->len] = 0;
    h->count++;
}

static void test_hpack_integers(void) {
    // RFC 7541 C.1: 10 in a 5-bit prefix, 1337 in a 5-bit prefix, 42 in 8.
    uint8_t buf[8]; uint32_t v = 0; size_t used = 0;
    CHECK_EQ_INT((int)Hpack_EncodeInt(buf, sizeof buf, 0, 5, 10), 1);
    CHECK_EQ_INT(buf[0], 0x0A);
    CHECK_EQ_INT((int)Hpack_EncodeInt(buf, sizeof buf, 0, 5, 1337), 3);
    CHECK(buf[0] == 0x1F && buf[1] == 0x9A && buf[2] == 0x0A);
    CHECK_EQ_INT(Hpack_DecodeInt(buf, 3, 5, &v, &used), 0);
    CHECK_EQ_INT((int)v, 1337);
    CHECK_EQ_INT((int)used, 3);
    CHECK_EQ_INT((int)Hpack_EncodeInt(buf, sizeof buf, 0, 8, 42), 1);
    CHECK_EQ_INT(buf[0], 42);
    // Prefix bits survive, and a truncated continuation is rejected.
    CHECK_EQ_INT((int)Hpack_EncodeInt(buf, sizeof buf, 0x80, 7, 200), 2);
    CHECK(buf[0] == 0xFF && buf[1] == 73);
    CHECK_EQ_INT(Hpack_DecodeInt(buf, 1, 7, &v, &used), -1);
}

static void test_hpack_huffman_table(void) {
    // Kraft equality: a complete prefix code sums 2^-len to exactly 1.
    uint64_t kraft = 0;
    int seen_len[32] = { 0 };
    for (int s = 0; s <= 256; s++) {
        uint32_t code = 0; int len = 0;
        Hpack_TestHuffmanEntry(s, &code, &len);
        CHECK(len >= 5 && len <= 30);
        CHECK(code < (1u << len));
        kraft += 1ull << (30 - len);
        seen_len[len] = 1;
    }
    CHECK(kraft == (1ull << 30));
    // Canonical: within one length codes rise by one in symbol order, and
    // the first code of the next used length continues from the last one.
    uint32_t prev_code = 0; int prev_len = 0, first = 1;
    for (int len = 5; len <= 30; len++) {
        if (!seen_len[len]) continue;
        for (int s = 0; s <= 256; s++) {
            uint32_t code = 0; int l = 0;
            Hpack_TestHuffmanEntry(s, &code, &l);
            if (l != len) continue;
            if (first) { CHECK_EQ_INT((int)code, 0); first = 0; }
            else if (prev_len == len) { CHECK(code == prev_code + 1); }
            else { CHECK(code == (prev_code + 1) << (len - prev_len)); }
            prev_code = code; prev_len = len;
        }
    }
}

static void test_hpack_huffman_strings(void) {
    // RFC 7541 C.4.1 / C.4.2 / C.4.3 / C.6.1 value strings.
    static const uint8_t www[]  = { 0xf1,0xe3,0xc2,0xe5,0xf2,0x3a,0x6b,0xa0,0xab,0x90,0xf4,0xff };
    static const uint8_t nc[]   = { 0xa8,0xeb,0x10,0x64,0x9c,0xbf };
    static const uint8_t ck[]   = { 0x25,0xa8,0x49,0xe9,0x5b,0xa9,0x7d,0x7f };
    static const uint8_t cv[]   = { 0x25,0xa8,0x49,0xe9,0x5b,0xb8,0xe8,0xb4,0xbf };
    static const uint8_t s302[] = { 0x64,0x02 };
    static const uint8_t priv[] = { 0xae,0xc3,0x77,0x1a,0x4b };
    static const uint8_t date[] = { 0xd0,0x7a,0xbe,0x94,0x10,0x54,0xd4,0x44,0xa8,0x20,0x05,0x95,
                                    0x04,0x0b,0x81,0x66,0xe0,0x82,0xa6,0x2d,0x1b,0xff };
    static const uint8_t loc[]  = { 0x9d,0x29,0xad,0x17,0x18,0x63,0xc7,0x8f,0x0b,0x97,0xc8,0xe9,
                                    0xae,0x82,0xae,0x43,0xd3 };
    struct { const uint8_t *in; size_t n; const char *want; } cases[] = {
        { www, sizeof www, "www.example.com" }, { nc, sizeof nc, "no-cache" },
        { ck, sizeof ck, "custom-key" }, { cv, sizeof cv, "custom-value" },
        { s302, sizeof s302, "302" }, { priv, sizeof priv, "private" },
        { date, sizeof date, "Mon, 21 Oct 2013 20:13:21 GMT" },
        { loc, sizeof loc, "https://www.example.com" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t out[64]; size_t n = 0;
        CHECK_EQ_INT(Hpack_HuffmanDecode(cases[i].in, cases[i].n, out, sizeof out, &n), 0);
        CHECK(n == strlen(cases[i].want) && memcmp(out, cases[i].want, n) == 0);
    }
    // Padding rules: eight or more pad bits, or pad bits that are not ones.
    static const uint8_t pad_too_long[] = { 0x64,0x02,0xff };   // "302" + 8 ones
    static const uint8_t pad_not_ones[] = { 0x1c };             // 'a' + 100
    uint8_t out[8]; size_t n = 0;
    CHECK_EQ_INT(Hpack_HuffmanDecode(pad_too_long, sizeof pad_too_long, out, sizeof out, &n), -1);
    CHECK_EQ_INT(Hpack_HuffmanDecode(pad_not_ones, sizeof pad_not_ones, out, sizeof out, &n), -1);
}

static void test_hpack_decode_blocks(void) {
    HpackDecoder *d = (HpackDecoder *)malloc(sizeof *d);
    CHECK(d != NULL);
    if (!d) return;
    HdrLog h;
    Hpack_Init(d);

    // C.3.1 -- C.3.3: literal requests, one decoder across the sequence.
    static const uint8_t c31[] = { 0x82,0x86,0x84,0x41,0x0f,'w','w','w','.','e','x','a','m','p','l','e','.','c','o','m' };
    static const uint8_t c32[] = { 0x82,0x86,0x84,0xbe,0x58,0x08,'n','o','-','c','a','c','h','e' };
    static const uint8_t c33[] = { 0x82,0x87,0x85,0xbf,0x40,0x0a,'c','u','s','t','o','m','-','k','e','y',
                                   0x0c,'c','u','s','t','o','m','-','v','a','l','u','e' };
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c31, sizeof c31, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n") == 0);
    CHECK_EQ_INT(d->count, 1);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c32, sizeof c32, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\ncache-control: no-cache\n") == 0);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c33, sizeof c33, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":method: GET\n:scheme: https\n:path: /index.html\n:authority: www.example.com\ncustom-key: custom-value\n") == 0);
    CHECK_EQ_INT(d->count, 3);
    CHECK_EQ_INT((int)(d->used + 32 * 3), 164);           // RFC: table size 164

    // C.4.1 -- C.4.3: the same requests Huffman-coded, fresh decoder.
    Hpack_Init(d);
    static const uint8_t c41[] = { 0x82,0x86,0x84,0x41,0x8c,0xf1,0xe3,0xc2,0xe5,0xf2,0x3a,0x6b,0xa0,0xab,0x90,0xf4,0xff };
    static const uint8_t c42[] = { 0x82,0x86,0x84,0xbe,0x58,0x86,0xa8,0xeb,0x10,0x64,0x9c,0xbf };
    static const uint8_t c43[] = { 0x82,0x87,0x85,0xbf,0x40,0x88,0x25,0xa8,0x49,0xe9,0x5b,0xa9,0x7d,0x7f,
                                   0x89,0x25,0xa8,0x49,0xe9,0x5b,0xb8,0xe8,0xb4,0xbf };
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c41, sizeof c41, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":method: GET\n:scheme: http\n:path: /\n:authority: www.example.com\n") == 0);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c42, sizeof c42, hdrlog_add, &h), 0);
    CHECK(strstr(h.text, "cache-control: no-cache\n") != NULL);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c43, sizeof c43, hdrlog_add, &h), 0);
    CHECK(strstr(h.text, "custom-key: custom-value\n") != NULL);
    CHECK_EQ_INT(d->count, 3);

    // C.5.1 -- C.5.3: responses with a 256-octet table (evictions). The
    // blocks are plain literals, so they are spelled out as text.
    Hpack_Init(d);
    d->max = 256;
    static const uint8_t c51[] = "\x48\x03" "302" "\x58\x07" "private"
                                 "\x61\x1d" "Mon, 21 Oct 2013 20:13:21 GMT"
                                 "\x6e\x17" "https://www.example.com";
    static const uint8_t c52[] = "\x48\x03" "307" "\xc1\xc0\xbf";
    static const uint8_t c53[] = "\x88\xc1" "\x61\x1d" "Mon, 21 Oct 2013 20:13:22 GMT"
                                 "\xc0" "\x5a\x04" "gzip"
                                 "\x77\x38" "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1";
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c51, sizeof c51 - 1, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":status: 302\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n") == 0);
    CHECK_EQ_INT(d->count, 4);
    CHECK_EQ_INT((int)(d->used + 32 * 4), 222);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c52, sizeof c52 - 1, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":status: 307\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:21 GMT\nlocation: https://www.example.com\n") == 0);
    CHECK_EQ_INT(d->count, 4);                            // 302 evicted for 307
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, c53, sizeof c53 - 1, hdrlog_add, &h), 0);
    CHECK(strcmp(h.text, ":status: 200\ncache-control: private\ndate: Mon, 21 Oct 2013 20:13:22 GMT\nlocation: https://www.example.com\ncontent-encoding: gzip\nset-cookie: foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1\n") == 0);
    CHECK_EQ_INT(d->count, 3);
    CHECK_EQ_INT((int)(d->used + 32 * 3), 215);

    // Malformed blocks: an index past the table, a string overrunning the
    // block, a table-size update above the negotiated limit.
    static const uint8_t bad_idx[] = { 0xff, 0x7f };
    static const uint8_t bad_str[] = { 0x40, 0x05, 'a', 'b' };
    static const uint8_t bad_upd[] = { 0x3f, 0xe2, 0x1f };             // 4097
    Hpack_Init(d);
    memset(&h, 0, sizeof h);
    CHECK_EQ_INT(Hpack_Decode(d, bad_idx, sizeof bad_idx, hdrlog_add, &h), -1);
    CHECK_EQ_INT(Hpack_Decode(d, bad_str, sizeof bad_str, hdrlog_add, &h), -1);
    CHECK_EQ_INT(Hpack_Decode(d, bad_upd, sizeof bad_upd, hdrlog_add, &h), -1);
    free(d);
}

// A scripted peer: reads come from `in` (optionally in tiny pieces), writes
// are captured in `out`.
typedef struct {
    const uint8_t *in; size_t in_len, in_pos, max_read;
    uint8_t out[4096]; size_t out_len;
} FakeIo;
static int fake_read(void *ctx, uint8_t *buf, size_t len) {
    FakeIo *f = (FakeIo *)ctx;
    size_t left = f->in_len - f->in_pos;
    if (!left) return 0;
    size_t n = len < left ? len : left;
    if (f->max_read && n > f->max_read) n = f->max_read;
    memcpy(buf, f->in + f->in_pos, n);
    f->in_pos += n;
    return (int)n;
}
static int fake_write(void *ctx, const uint8_t *buf, size_t len) {
    FakeIo *f = (FakeIo *)ctx;
    if (f->out_len + len > sizeof f->out) return -1;
    memcpy(f->out + f->out_len, buf, len);
    f->out_len += len;
    return (int)len;
}
static size_t script_frame(uint8_t *dst, size_t cap, size_t pos, uint8_t type,
                           uint8_t flags, uint32_t sid, const uint8_t *pl, size_t n) {
    if (pos + 9 + n > cap) return pos;
    dst[pos]     = (uint8_t)(n >> 16); dst[pos + 1] = (uint8_t)(n >> 8); dst[pos + 2] = (uint8_t)n;
    dst[pos + 3] = type;               dst[pos + 4] = flags;
    dst[pos + 5] = (uint8_t)((sid >> 24) & 0x7f); dst[pos + 6] = (uint8_t)(sid >> 16);
    dst[pos + 7] = (uint8_t)(sid >> 8);           dst[pos + 8] = (uint8_t)sid;
    if (n) memcpy(dst + pos + 9, pl, n);
    return pos + 9 + n;
}

// Walk the frames the client wrote after the 24-byte preface.
typedef struct { uint8_t type, flags; uint32_t sid; const uint8_t *pl; size_t len; } SeenFrame;
static int walk_client_frames(const FakeIo *f, SeenFrame *out, int cap) {
    int n = 0;
    size_t pos = 24;
    while (pos + 9 <= f->out_len && n < cap) {
        const uint8_t *fh = f->out + pos;
        size_t len = ((size_t)fh[0] << 16) | ((size_t)fh[1] << 8) | fh[2];
        if (pos + 9 + len > f->out_len) break;
        out[n].type = fh[3]; out[n].flags = fh[4];
        out[n].sid = ((uint32_t)(fh[5] & 0x7f) << 24) | ((uint32_t)fh[6] << 16) |
                     ((uint32_t)fh[7] << 8) | fh[8];
        out[n].pl = fh + 9; out[n].len = len;
        n++;
        pos += 9 + len;
    }
    return n;
}

static void test_h2_post_once(void) {
    // Server script: SETTINGS, PING, WINDOW_UPDATE, SETTINGS ACK, an interim
    // 103, HEADERS(:status 200 + content-type) split over a CONTINUATION,
    // a padded DATA frame, a final DATA with END_STREAM, then trailing junk
    // the client must never read.
    static uint8_t script[1024];
    size_t sp = 0;
    static const uint8_t srv_settings[] = { 0,3, 0,0,0,100 };
    static const uint8_t ping[8]        = { 1,2,3,4,5,6,7,8 };
    static const uint8_t wu[4]          = { 0,0,0x10,0 };
    static const uint8_t h103[]         = { 0x08, 0x03, '1','0','3' };
    static const uint8_t h200a[]        = { 0x88 };
    static const uint8_t h200b[]        = { 0x0f, 0x10, 0x17, 'a','p','p','l','i','c','a','t','i','o','n',
                                            '/','d','n','s','-','m','e','s','s','a','g','e' };
    static const uint8_t d1[]           = { 3, 'h','e','l', 0,0,0 };   // PADDED: 3 pad octets
    static const uint8_t d2[]           = { 'l','o' };
    static const uint8_t junk[]         = { 0xde,0xad };
    sp = script_frame(script, sizeof script, sp, 4, 0,    0, srv_settings, sizeof srv_settings);
    sp = script_frame(script, sizeof script, sp, 6, 0,    0, ping, 8);
    sp = script_frame(script, sizeof script, sp, 8, 0,    0, wu, 4);
    sp = script_frame(script, sizeof script, sp, 4, 0x1,  0, NULL, 0);
    sp = script_frame(script, sizeof script, sp, 1, 0x4,  1, h103, sizeof h103);
    sp = script_frame(script, sizeof script, sp, 1, 0,    1, h200a, sizeof h200a);
    sp = script_frame(script, sizeof script, sp, 9, 0x4,  1, h200b, sizeof h200b);
    sp = script_frame(script, sizeof script, sp, 0, 0x8,  1, d1, sizeof d1);
    sp = script_frame(script, sizeof script, sp, 0, 0x1,  1, d2, sizeof d2);
    size_t script_end = sp;
    sp = script_frame(script, sizeof script, sp, 0, 0x1,  1, junk, sizeof junk);

    static const uint8_t body[] = "QUERY";
    for (int pass = 0; pass < 2; pass++) {
        FakeIo f; memset(&f, 0, sizeof f);
        f.in = script; f.in_len = sp; f.max_read = pass ? 1 : 0;   // pass 1: byte-wise reads
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[64]; size_t got = 0; int status = 0;
        CHECK_EQ_INT(H2_PostOnce(&io, "dns.example", "/dns-query", "application/dns-message",
                                 body, 5, out, sizeof out, &got, &status), 0);
        CHECK_EQ_INT(status, 200);
        CHECK(got == 5 && memcmp(out, "hello", 5) == 0);
        CHECK(f.in_pos == script_end);                 // stopped at END_STREAM

        // What the client sent: preface, SETTINGS, HEADERS, DATA, then the
        // SETTINGS ACK, the PING ACK, window updates for the padded DATA,
        // and a GOAWAY.
        CHECK(f.out_len > 24 && memcmp(f.out, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0);
        SeenFrame fr[16];
        int nf = walk_client_frames(&f, fr, 16);
        CHECK(nf >= 7);
        if (nf >= 7) {
            CHECK(fr[0].type == 4 && fr[0].flags == 0 && fr[0].sid == 0 && fr[0].len == 18);
            CHECK(fr[1].type == 1 && fr[1].flags == 0x4 && fr[1].sid == 1);
            CHECK(fr[2].type == 0 && fr[2].flags == 0x1 && fr[2].sid == 1 &&
                  fr[2].len == 5 && memcmp(fr[2].pl, "QUERY", 5) == 0);
            // The request header block decodes to exactly the seven fields.
            HpackDecoder *d = (HpackDecoder *)malloc(sizeof *d);
            HdrLog h; memset(&h, 0, sizeof h);
            if (d) {
                Hpack_Init(d);
                CHECK_EQ_INT(Hpack_Decode(d, fr[1].pl, fr[1].len, hdrlog_add, &h), 0);
                CHECK(strcmp(h.text,
                      ":method: POST\n:scheme: https\n:authority: dns.example\n:path: /dns-query\n"
                      "content-type: application/dns-message\naccept: application/dns-message\n"
                      "content-length: 5\n") == 0);
                free(d);
            }
            int saw_settings_ack = 0, saw_ping_ack = 0, saw_wu = 0, saw_goaway = 0;
            for (int i = 3; i < nf; i++) {
                if (fr[i].type == 4 && fr[i].flags == 0x1 && fr[i].len == 0) saw_settings_ack = 1;
                if (fr[i].type == 6 && fr[i].flags == 0x1 && fr[i].len == 8 &&
                    memcmp(fr[i].pl, ping, 8) == 0) saw_ping_ack = 1;
                if (fr[i].type == 8 && fr[i].len == 4) saw_wu++;
                if (fr[i].type == 7) saw_goaway = 1;
            }
            CHECK(saw_settings_ack && saw_ping_ack && saw_wu == 2 && saw_goaway);
        }
    }

    // Failure scripts: each must return -1 with an empty result.
    struct { const char *what; uint8_t type, flags; uint32_t sid; const uint8_t *pl; size_t n; } bad[] = {
        { "RST_STREAM on the stream", 3, 0, 1, (const uint8_t *)"\0\0\0\x8", 4 },
        { "GOAWAY", 7, 0, 0, (const uint8_t *)"\0\0\0\0\0\0\0\0", 8 },
        { "DATA before HEADERS", 0, 0x1, 1, d2, sizeof d2 },
        { "PUSH_PROMISE", 5, 0x4, 1, (const uint8_t *)"\0\0\0\x2", 4 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t s2[256]; size_t p2 = 0;
        p2 = script_frame(s2, sizeof s2, p2, 4, 0, 0, NULL, 0);
        p2 = script_frame(s2, sizeof s2, p2, bad[i].type, bad[i].flags, bad[i].sid, bad[i].pl, bad[i].n);
        FakeIo f; memset(&f, 0, sizeof f);
        f.in = s2; f.in_len = p2;
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[64]; size_t got = 1; int status = 1;
        CHECK_EQ_INT(H2_PostOnce(&io, "dns.example", "/dns-query", "application/dns-message",
                                 body, 5, out, sizeof out, &got, &status), -1);
        CHECK(got == 0 && status == 0);
    }
    // An oversized frame, a body beyond the caller's buffer, and EOF before
    // the response completes.
    {
        uint8_t s2[64] = { 0x00, 0x40, 0x01, 4, 0, 0,0,0,0 };   // length 16385
        FakeIo f; memset(&f, 0, sizeof f); f.in = s2; f.in_len = 9;
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[64]; size_t got = 0; int status = 0;
        CHECK_EQ_INT(H2_PostOnce(&io, "a", "/", "t", body, 5, out, sizeof out, &got, &status), -1);
    }
    {
        uint8_t s2[128]; size_t p2 = 0;
        static const uint8_t big[20] = { 0 };
        p2 = script_frame(s2, sizeof s2, p2, 1, 0x4, 1, h200a, sizeof h200a);
        p2 = script_frame(s2, sizeof s2, p2, 0, 0x1, 1, big, sizeof big);
        FakeIo f; memset(&f, 0, sizeof f); f.in = s2; f.in_len = p2;
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[8]; size_t got = 0; int status = 0;
        CHECK_EQ_INT(H2_PostOnce(&io, "a", "/", "t", body, 5, out, sizeof out, &got, &status), -1);
    }
    {
        uint8_t s2[128]; size_t p2 = 0;
        p2 = script_frame(s2, sizeof s2, p2, 1, 0x4, 1, h200a, sizeof h200a);   // no DATA, no END_STREAM
        FakeIo f; memset(&f, 0, sizeof f); f.in = s2; f.in_len = p2;
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[8]; size_t got = 0; int status = 0;
        CHECK_EQ_INT(H2_PostOnce(&io, "a", "/", "t", body, 5, out, sizeof out, &got, &status), -1);
    }
    // A response with no body at all: HEADERS carrying END_STREAM.
    {
        uint8_t s2[128]; size_t p2 = 0;
        static const uint8_t h204[] = { 0x89 };
        p2 = script_frame(s2, sizeof s2, p2, 1, 0x5, 1, h204, sizeof h204);
        FakeIo f; memset(&f, 0, sizeof f); f.in = s2; f.in_len = p2;
        H2Io io = { fake_read, fake_write, &f };
        uint8_t out[8]; size_t got = 9; int status = 0;
        CHECK_EQ_INT(H2_PostOnce(&io, "a", "/", "t", NULL, 0, out, sizeof out, &got, &status), 0);
        CHECK(status == 204 && got == 0);
    }
}

static void test_logbuf_collect_wrapped(void) {
    // Overflow the ring so the overwrite-oldest branch runs and the head
    // moves: collection must stay in append order with contiguous seq,
    // and the seq gap at the front (the Tcl drain's loss detector) must
    // equal exactly the number of overwritten entries.
    Log_Reset();
    enum { EXTRA = 5 };
    for (int i = 0; i < LOGBUF_CAP + EXTRA; i++) {
        Log_Append("wrap-%d", i);
    }

    static LogRawEntry got[LOGBUF_CAP];
    size_t n = Log_CollectSince(0, got, LOGBUF_CAP);
    CHECK_EQ_INT((int)n, LOGBUF_CAP);

    // Oldest survivor is the (EXTRA+1)-th append; the newest is the last.
    char expectFirst[32], expectLast[32];
    snprintf(expectFirst, sizeof expectFirst, "wrap-%d", EXTRA);
    snprintf(expectLast, sizeof expectLast, "wrap-%d", LOGBUF_CAP + EXTRA - 1);
    CHECK(strcmp(got[0].msg, expectFirst) == 0);
    CHECK(strcmp(got[n - 1].msg, expectLast) == 0);

    // seq strictly contiguous across the whole wrapped collection, and
    // the front gap equals the EXTRA entries the overwrite consumed.
    int contiguous = 1;
    for (size_t i = 1; i < n; i++) {
        if (got[i].seq != got[i - 1].seq + 1) { contiguous = 0; break; }
    }
    CHECK(contiguous);
    CHECK(got[0].seq == got[n - 1].seq - (uint64_t)(LOGBUF_CAP - 1));

    // sinceSeq inside the wrapped region still returns only the suffix.
    static LogRawEntry sfx[LOGBUF_CAP];
    uint64_t mid = got[n - 3].seq;
    size_t m = Log_CollectSince(mid, sfx, LOGBUF_CAP);
    CHECK_EQ_INT((int)m, 2);
    CHECK(sfx[0].seq == mid + 1);

    Log_Reset();   // leave a clean ring for later suites
}

// ---------------------------------------------------------------------------
// NTS provider pool -- metadata only, PickProvider returns an entry
// ---------------------------------------------------------------------------

#include "../src/nts.h"

// Update-check version comparison (drives the "is a newer release
// available" decision): numeric, per-component, missing = 0.
static void test_update_version_cmp(void) {
    CHECK(UpdateCheck_VersionCmp("0.5.0", "0.4.0") > 0);
    CHECK(UpdateCheck_VersionCmp("0.4.0", "0.5.0") < 0);
    CHECK(UpdateCheck_VersionCmp("0.4.0", "0.4.0") == 0);
    CHECK(UpdateCheck_VersionCmp("0.4", "0.4.0") == 0);       // missing = 0
    CHECK(UpdateCheck_VersionCmp("1.0.0", "0.99.99") > 0);
    CHECK(UpdateCheck_VersionCmp("0.10.0", "0.9.0") > 0);     // numeric, not lexical
    CHECK(UpdateCheck_VersionCmp("0.4.1", "0.4.0") > 0);
}

static void test_nts_cookie_jar(void) {
    Nts_TestJarReset();

    // No jar for an unknown host.
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), -1);
    CHECK_EQ_INT(Nts_TestJarTake("a.example"), 0);

    // Store 8 cookies; taking spends one at a time.
    Nts_TestJarStore("a.example", 8);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 8);
    CHECK_EQ_INT(Nts_TestJarTake("a.example"), 1);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 7);

    // Harvest tops the jar back up, capped at the 8-cookie ceiling.
    Nts_TestJarAdd("a.example", 3);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 8);   // 7 + 3 -> capped at 8

    // Drain to empty; take then fails and the jar reports empty (not -1:
    // the jar still exists, it just has no cookies).
    for (int i = 0; i < 8; i++) CHECK_EQ_INT(Nts_TestJarTake("a.example"), 1);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 0);
    CHECK_EQ_INT(Nts_TestJarTake("a.example"), 0);

    // Distinct hosts have independent jars.
    Nts_TestJarStore("b.example", 4);
    Nts_TestJarStore("a.example", 2);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 2);
    CHECK_EQ_INT(Nts_TestJarCount("b.example"), 4);

    // Drop removes the jar entirely (-1, not 0).
    Nts_TestJarDrop("b.example");
    CHECK_EQ_INT(Nts_TestJarCount("b.example"), -1);
    CHECK_EQ_INT(Nts_TestJarCount("a.example"), 2);   // unaffected

    Nts_TestJarReset();
}

static void test_nts_pool_pins(void) {
    // The shipped binary must not contain provider cryptographic
    // material. The pool is endpoint/operator metadata only; SPKI pins
    // are enrolled into the local protected pin store at runtime.
    size_t n = 0;
    const NtsProvider *p = Nts_Pool(&n);
    CHECK(p != NULL);
    CHECK(n > 0);

    int n_families = 0;
    for (size_t i = 0; i < n; i++) {
        CHECK(p[i].host != NULL && p[i].host[0] != 0);
        CHECK(p[i].label != NULL);
        CHECK(p[i].operator_family != NULL && p[i].operator_family[0] != 0);
        int first = 1;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(p[i].operator_family, p[j].operator_family) == 0) first = 0;
        }
        if (first) n_families++;
    }
    CHECK(n_families >= 2);

    // PickProvider must return an entry from the metadata pool.
    const NtsProvider *picked = Nts_PickProvider();
    CHECK(picked != NULL);
    if (picked) {
        CHECK(picked->operator_family != NULL && picked->operator_family[0] != 0);
        int found = 0;
        for (size_t i = 0; i < n; i++) if (&p[i] == picked) found = 1;
        CHECK(found);
    }

    const NtsProvider *picked_many[4] = {0};
    size_t got = Nts_PickProviders(picked_many, 4);
    CHECK(got >= 2);
    for (size_t i = 0; i < got; i++) {
        CHECK(picked_many[i] != NULL);
        for (size_t j = i + 1; j < got; j++) CHECK(picked_many[i] != picked_many[j]);
    }
}

// ---------------------------------------------------------------------------
// IANA resolver: edge zones + malformed-input defense
// ---------------------------------------------------------------------------

static void test_tz_bounds(void) {
    // Version string is present and non-empty.
    const char *ver = Tz_Version();
    CHECK(ver != NULL);
    CHECK(ver[0] != 0);

    // UTC is always at index 0 and resolves back to its name.
    CHECK_EQ_STR(Tz_AtIndex(0), "UTC");
    CHECK_EQ_STR(Tz_Name(TZ_ID_UTC), "UTC");

    // Tz_Count must match the range of valid indices.
    int n = Tz_Count();
    CHECK(n >= 100);  // sanity: at least a hundred canonical zones
    CHECK(Tz_AtIndex(n - 1) != NULL);

    // Out-of-range lookups return NULL without walking off the table.
    CHECK(Tz_AtIndex(-1) == NULL);
    CHECK(Tz_AtIndex(n)   == NULL);
    CHECK(Tz_AtIndex(n + 1000) == NULL);
    CHECK(Tz_Name(TZ_ID_INVALID) == NULL);
    CHECK(Tz_Name((TzId)(n + 1))  == NULL);

    // NULL / empty names return invalid.
    CHECK_EQ_INT(Tz_FindByName(NULL), TZ_ID_INVALID);
    CHECK_EQ_INT(Tz_FindByName(""),   TZ_ID_INVALID);

    // Resolve with invalid id returns 0 (failure), does not crash.
    TzifLocal tl;
    CHECK_EQ_INT(Tz_LocalFromUtcMs(TZ_ID_INVALID, 0, &tl), 0);
    CHECK_EQ_INT(Tz_LocalFromUtcMs((TzId)(n + 1), 0, &tl), 0);
    // NULL out also rejected.
    CHECK_EQ_INT(Tz_LocalFromUtcMs(TZ_ID_UTC, 0, NULL), 0);
}

static void test_tz_southern_hemisphere(void) {
    // Southern-hemisphere zones are a stress-test for the POSIX-footer
    // resolver: DST runs Oct->Apr, so the "end" rule falls in the
    // *next* calendar year relative to the "start". rule_local_sec
    // must be evaluated for Y-1/Y/Y+1 and the latest applicable one
    // picked -- otherwise Jan 15 resolves to "not yet in DST".
    TzifLocal tl;

    // 2026-01-15 12:00:00Z -> Sydney should be AEDT +11:00 (summer/DST).
    TzId syd = Tz_FindByName("Australia/Sydney");
    CHECK(syd != TZ_ID_INVALID);
    int64_t jan = make_utc_ms(2026, 1, 15, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(syd, jan, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 11 * 3600);
    CHECK_EQ_STR(tl.abbr, "AEDT");
    CHECK_EQ_INT(tl.isDst, 1);
    CHECK_EQ_INT(tl.hour, 23);         // 12Z + 11h
    CHECK_EQ_INT(tl.mday, 15);

    // 2026-07-15 12:00:00Z -> Sydney should be AEST +10:00 (winter).
    int64_t jul = make_utc_ms(2026, 7, 15, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(syd, jul, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 10 * 3600);
    CHECK_EQ_STR(tl.abbr, "AEST");
    CHECK_EQ_INT(tl.isDst, 0);

    // 2026-01-15 -> Auckland NZDT +13:00 (summer/DST).
    TzId akl = Tz_FindByName("Pacific/Auckland");
    CHECK(akl != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(akl, jan, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 13 * 3600);
    CHECK_EQ_STR(tl.abbr, "NZDT");
    CHECK_EQ_INT(tl.isDst, 1);

    // 2026-07-15 -> Auckland NZST +12:00 (winter).
    CHECK(Tz_LocalFromUtcMs(akl, jul, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 12 * 3600);
    CHECK_EQ_STR(tl.abbr, "NZST");
    CHECK_EQ_INT(tl.isDst, 0);
}

static void test_tz_half_hour_zones(void) {
    TzifLocal tl;
    int64_t t = make_utc_ms(2026, 6, 15, 12, 0, 0);

    // India Standard Time: +05:30 year-round, no DST.
    TzId kol = Tz_FindByName("Asia/Kolkata");
    CHECK(kol != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(kol, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 5 * 3600 + 30 * 60);
    CHECK_EQ_STR(tl.abbr, "IST");
    CHECK_EQ_INT(tl.isDst, 0);
    CHECK_EQ_INT(tl.hour, 17);
    CHECK_EQ_INT(tl.minute, 30);

    // Nepal: +05:45, the only 45-minute offset still in use.
    TzId ktm = Tz_FindByName("Asia/Kathmandu");
    CHECK(ktm != TZ_ID_INVALID);
    CHECK(Tz_LocalFromUtcMs(ktm, t, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 5 * 3600 + 45 * 60);
    CHECK_EQ_INT(tl.hour, 17);
    CHECK_EQ_INT(tl.minute, 45);

    // Adelaide (South Australia): ACDT +10:30 in summer, ACST +09:30
    // in winter.  Pick both sides to exercise the half-hour + DST
    // combination.
    TzId adl = Tz_FindByName("Australia/Adelaide");
    CHECK(adl != TZ_ID_INVALID);
    int64_t jan = make_utc_ms(2026, 1, 15, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(adl, jan, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 10 * 3600 + 30 * 60);
    CHECK_EQ_INT(tl.isDst, 1);
    CHECK_EQ_STR(tl.abbr, "ACDT");

    int64_t jul = make_utc_ms(2026, 7, 15, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(adl, jul, &tl) == 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 9 * 3600 + 30 * 60);
    CHECK_EQ_INT(tl.isDst, 0);
    CHECK_EQ_STR(tl.abbr, "ACST");
}

static void test_tz_all_zones_smoke(void) {
    // Iterate every embedded zone and assert a resolution at a single
    // arbitrary "now" produces plausible values.  Catches a corrupt
    // blob table, a bad index, or any zone whose footer our parser
    // rejects.
    int64_t t = make_utc_ms(2026, 6, 15, 12, 0, 0);
    int n = Tz_Count();
    int failures = 0;
    for (int i = 0; i < n; i++) {
        TzifLocal tl;
        const char *name = Tz_AtIndex(i);
        if (!name) { failures++; continue; }
        if (Tz_LocalFromUtcMs((TzId)i, t, &tl) != 1) { failures++; continue; }

        // Offsets on Earth today fall between -12:00 (Baker Island) and
        // +14:00 (Kiribati). Allow a small margin for LMT historical
        // outliers just in case.
        if (tl.utcOffsetSec < -13 * 3600 || tl.utcOffsetSec > 15 * 3600) failures++;
        if (tl.year  < 2025 || tl.year  > 2027) failures++;
        if (tl.month < 1    || tl.month > 12)   failures++;
        if (tl.mday  < 1    || tl.mday  > 31)   failures++;
        if (tl.hour  < 0    || tl.hour  > 23)   failures++;
        if (tl.minute < 0   || tl.minute > 59)  failures++;
        if (tl.wday  < 0    || tl.wday  > 6)    failures++;
        if (tl.yday  < 0    || tl.yday  > 365)  failures++;
        if (tl.abbr[TZIF_ABBR_CAP - 1] != 0)    failures++;   // must be NUL
        if (tl.abbr[0] == 0)                    failures++;   // non-empty
    }
    CHECK_EQ_INT(failures, 0);
}

static void test_tzif_malformed(void) {
    // Defense in depth: even though we ship the blob ourselves, the
    // parser must reject a truncated / corrupted input without reading
    // past bounds or crashing.
    TzifLocal tl;
    uint8_t buf[256];

    // NULL blob.
    CHECK_EQ_INT(tzif_resolve(NULL, 128, 0, &tl), 0);

    // Blob too short for even a header.
    memset(buf, 0, sizeof buf);
    CHECK_EQ_INT(tzif_resolve(buf, 10, 0, &tl), 0);

    // Correct length but wrong magic.
    memset(buf, 0, sizeof buf);
    memcpy(buf, "NOPE", 4);
    buf[4] = '2';
    CHECK_EQ_INT(tzif_resolve(buf, 44, 0, &tl), 0);

    // TZif but version '1' (unsupported -- modern zic always writes 2+).
    memset(buf, 0, sizeof buf);
    memcpy(buf, "TZif", 4);
    buf[4] = '1';
    CHECK_EQ_INT(tzif_resolve(buf, 44, 0, &tl), 0);

    // TZif version '2' with header claiming nonzero counts but buffer
    // too small to contain the v1 data block.
    memset(buf, 0, sizeof buf);
    memcpy(buf, "TZif", 4);
    buf[4] = '2';
    // 6 counts at offset 20..44.  Set tcnt=100, typec=10, charc=100
    // (needs hundreds of bytes we don't have).
    buf[20 + 12 + 0] = 0; buf[20 + 12 + 1] = 0;
    buf[20 + 12 + 2] = 0; buf[20 + 12 + 3] = 100;
    buf[20 + 16 + 0] = 0; buf[20 + 16 + 1] = 0;
    buf[20 + 16 + 2] = 0; buf[20 + 16 + 3] = 10;
    buf[20 + 20 + 0] = 0; buf[20 + 20 + 1] = 0;
    buf[20 + 20 + 2] = 0; buf[20 + 20 + 3] = 100;
    CHECK_EQ_INT(tzif_resolve(buf, 44, 0, &tl), 0);

    // Truncated right after v1 magic but before v2.
    memset(buf, 0, sizeof buf);
    memcpy(buf, "TZif", 4); buf[4] = '2';
    // Claim zero counts in v1 so v1 end is at 44.
    CHECK_EQ_INT(tzif_resolve(buf, 60, 0, &tl), 0);   // v2 header can't fit

    // Legitimate v1 empty + v2 missing magic.
    memset(buf, 0, sizeof buf);
    memcpy(buf, "TZif", 4); buf[4] = '2';
    // v1 empty -> v2 expected at offset 44; leave it zero (wrong magic).
    CHECK_EQ_INT(tzif_resolve(buf, 256, 0, &tl), 0);
}

// ---------------------------------------------------------------------------
// Calendar breakdown: proleptic Gregorian math via Tz_LocalFromUtcMs(UTC)
// ---------------------------------------------------------------------------

static void test_breakdown_leap_years(void) {
    TzifLocal tl;
    // 1970-01-01 00:00:00Z was a Thursday (wday=4).
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, 0, &tl) == 1);
    CHECK_EQ_INT(tl.year, 1970);
    CHECK_EQ_INT(tl.month, 1);
    CHECK_EQ_INT(tl.mday, 1);
    CHECK_EQ_INT(tl.wday, 4);
    CHECK_EQ_INT(tl.yday, 0);

    // 1969-12-31 23:59:59Z -- one second before the epoch.  Must break
    // down to Dec 31 1969, not wrap.
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, -1000, &tl) == 1);
    CHECK_EQ_INT(tl.year, 1969);
    CHECK_EQ_INT(tl.month, 12);
    CHECK_EQ_INT(tl.mday, 31);
    CHECK_EQ_INT(tl.hour, 23);
    CHECK_EQ_INT(tl.minute, 59);
    CHECK_EQ_INT(tl.second, 59);

    // 2000-02-29 12:00:00Z -- leap year (divisible by 400).
    int64_t t = make_utc_ms(2000, 2, 29, 12, 0, 0);
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.year, 2000);
    CHECK_EQ_INT(tl.month, 2);
    CHECK_EQ_INT(tl.mday, 29);
    CHECK_EQ_INT(tl.yday, 59);  // Jan(31) + Feb 29 offset = 31 + 28 = 59

    // 2024-02-29 -- ordinary leap year (divisible by 4, not 100).
    t = make_utc_ms(2024, 2, 29, 0, 0, 0);
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.year, 2024);
    CHECK_EQ_INT(tl.month, 2);
    CHECK_EQ_INT(tl.mday, 29);

    // 2100-03-01 00:00:00Z -- NOT a leap year (divisible by 100 but
    // not 400).  Feb 2100 has 28 days, so Mar 1 00:00 lies at the
    // correct offset.
    t = make_utc_ms(2100, 3, 1, 0, 0, 0);
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.year, 2100);
    CHECK_EQ_INT(tl.month, 3);
    CHECK_EQ_INT(tl.mday, 1);
    CHECK_EQ_INT(tl.yday, 59);  // Jan(31) + Feb(28) = 59

    // 2026-12-31 23:59:59Z -- end of year, yday == 364 (non-leap).
    t = make_utc_ms(2026, 12, 31, 23, 59, 59);
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.yday, 364);
    CHECK_EQ_INT(tl.wday, 4);   // 2026-12-31 is a Thursday

    // Leap-year-31-Dec: 2024-12-31 has yday == 365.
    t = make_utc_ms(2024, 12, 31, 0, 0, 0);
    CHECK(Tz_LocalFromUtcMs(TZ_ID_UTC, t, &tl) == 1);
    CHECK_EQ_INT(tl.yday, 365);
}

// ---------------------------------------------------------------------------
// POSIX TZ footer hardening: hostile or malformed strings must neither
// overflow nor hang.  Exercised through tzif_resolve on synthetic TZif
// blobs so we cover the same code path as the real resolver.
// ---------------------------------------------------------------------------

// Build a minimal valid v2/v3 TZif blob with zero explicit transitions
// and one typec entry, ending in a newline-delimited POSIX footer.
// Returns the byte count written into `out`; 0 on cap overflow.
static size_t build_empty_tzif_with_footer(uint8_t *out, size_t cap,
                                           const char *posix) {
    size_t flen = strlen(posix);
    // Layout:
    //   0..19  v1 header ("TZif3\0*15")  — v1 empty block: 6 counts all 0
    //   20..43 v1 counts (6 * u32be zeros), counts followed by no data
    //  44..63  v2 header "TZif3\0*15"
    //  64..87  v2 counts (tti_ut=tti_st=0, leap=0, tcnt=0, typec=1, charc=2)
    //  88..93  typec ttinfo (gmtoff=0, isdst=0, abbridx=0)
    //  94..95  abbrs "Z\0"
    //  96      '\n'
    //  97..    posix footer
    size_t need = 44 + 44 + 6 + 2 + 1 + flen + 1;   // final '\n'
    if (need > cap) return 0;
    memset(out, 0, need);
    memcpy(out + 0,  "TZif", 4); out[4]  = '3';
    memcpy(out + 44, "TZif", 4); out[48] = '3';
    // v2 counts: only typec=1 and charc=2 nonzero.
    out[44 + 20 + 16 + 3] = 0x01;   // typec = 1
    out[44 + 20 + 20 + 3] = 0x02;   // charc = 2
    // ttinfo (6 bytes): gmtoff(4)=0, isdst(1)=0, abbridx(1)=0
    // -> already zero from memset.
    // abbrs: "Z\0"
    out[88 + 6 + 0] = 'Z';
    out[88 + 6 + 1] = 0;
    out[44 + 44 + 6 + 2] = '\n';
    memcpy(out + 44 + 44 + 6 + 2 + 1, posix, flen);
    out[need - 1] = '\n';
    return need;
}

static void test_posix_footer_hardening(void) {
    uint8_t buf[1024];
    TzifLocal tl;

    // 1) Unbounded digit run in DST rule: "M99999999...9.1.0".  With
    //    take_digits capping at 2 hour digits / etc., this must neither
    //    overflow nor hang -- it clamps to an in-range rule and still
    //    produces a valid resolution.
    char footer1[256];
    char longdigits[128];
    for (int i = 0; i < 100; i++) longdigits[i] = '9';
    longdigits[100] = 0;
    snprintf(footer1, sizeof footer1,
             "EST5EDT,M%s.1.0,M11.1.0", longdigits);
    size_t n = build_empty_tzif_with_footer(buf, sizeof buf, footer1);
    CHECK(n > 0);
    // Resolve an instant in June; must succeed, must land on DST or
    // standard -- correctness of the synthetic zone matters less than
    // the absence of crash/hang.
    int64_t jun = make_utc_ms(2026, 6, 15, 12, 0, 0) / 1000;
    CHECK_EQ_INT(tzif_resolve(buf, n, jun, &tl), 1);

    // 2) Long offset digit run in the hour field: "EST<999999...9>EDT,..."
    char footer2[256];
    snprintf(footer2, sizeof footer2,
             "EST%s:30EDT4,M3.2.0,M11.1.0", longdigits);
    n = build_empty_tzif_with_footer(buf, sizeof buf, footer2);
    CHECK(n > 0);
    // Must resolve without overflow.  Clamped offset may be unusual
    // but must stay within our sanity envelope.
    CHECK_EQ_INT(tzif_resolve(buf, n, jun, &tl), 1);
    CHECK(tl.utcOffsetSec >= -15 * 3600 && tl.utcOffsetSec <= 15 * 3600);

    // 3) Out-of-range Mm.w.d components (M99.99.99).  Clamping inside
    //    parse_rule pulls them back into the legal domain; the rule
    //    still evaluates to *some* in-range instant.
    const char *footer3 = "XYZ3ABC,M99.99.99,M99.99.99";
    n = build_empty_tzif_with_footer(buf, sizeof buf, footer3);
    CHECK(n > 0);
    CHECK_EQ_INT(tzif_resolve(buf, n, jun, &tl), 1);

    // 4) Malformed rule (missing comma between start and end rules)
    //    -> parse_posix returns 0; hasFooter=0; resolver falls back
    //    to the single ttinfo entry ("Z" / gmtoff=0 / isdst=0).
    const char *footer4 = "XYZ3ABC,M3.2.0";   // no end rule
    n = build_empty_tzif_with_footer(buf, sizeof buf, footer4);
    CHECK(n > 0);
    CHECK_EQ_INT(tzif_resolve(buf, n, jun, &tl), 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 0);
    CHECK_EQ_INT(tl.isDst, 0);

    // 5) Empty footer -- parser must not trip on it.
    const char *footer5 = "";
    n = build_empty_tzif_with_footer(buf, sizeof buf, footer5);
    CHECK(n > 0);
    CHECK_EQ_INT(tzif_resolve(buf, n, jun, &tl), 1);
    CHECK_EQ_INT(tl.utcOffsetSec, 0);
}

// ---------------------------------------------------------------------------
// DNS-over-HTTPS resolver: wire format + pool invariants + agreement
// ---------------------------------------------------------------------------

static void test_dns_pool_pins(void) {
    // Same metadata-only invariant as test_nts_pool_pins but for DoH.
    size_t n = 0;
    const DnsResolver *p = Dns_Pool(&n);
    CHECK(p != NULL);
    CHECK(n >= 2);   // need at least two for secure DoH failover

    for (size_t i = 0; i < n; i++) {
        CHECK(p[i].hostname    != NULL && p[i].hostname[0]    != 0);
        CHECK(p[i].ip_primary  != NULL && p[i].ip_primary[0]  != 0);
        CHECK(p[i].label       != NULL && p[i].label[0]       != 0);
        CHECK(p[i].operator_family != NULL && p[i].operator_family[0] != 0);
    }
}

static void test_dns_pick_resolvers(void) {
    // Asking for 2 must yield 2 distinct resolvers. (The buffer must hold
    // the whole pool: the "more than the pool" case below clamps to it.)
    const DnsResolver *pick[16] = {0};
    size_t got = Dns_PickResolvers(pick, 2);
    CHECK_EQ_INT(got, 2);
    CHECK(pick[0] != NULL && pick[1] != NULL);
    CHECK(pick[0] != pick[1]);

    // Asking for more than the pool clamps down.
    got = Dns_PickResolvers(pick, 100);
    CHECK(got >= 2);
    // All returned entries must be distinct metadata entries.
    for (size_t i = 0; i < got; i++) {
        CHECK(pick[i] != NULL);
        CHECK(pick[i]->operator_family != NULL && pick[i]->operator_family[0] != 0);
        for (size_t j = i + 1; j < got; j++) {
            CHECK(pick[i] != pick[j]);
        }
    }

    // Zero-want returns zero, writes nothing.
    got = Dns_PickResolvers(pick, 0);
    CHECK_EQ_INT(got, 0);

    // NULL buffer returns zero.
    got = Dns_PickResolvers(NULL, 2);
    CHECK_EQ_INT(got, 0);
}

static void test_pin_store_roundtrip(void) {
    wchar_t oldAppData[MAX_PATH];
    DWORD oldLen = GetEnvironmentVariableW(L"APPDATA", oldAppData, MAX_PATH);
    wchar_t tempRoot[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, tempRoot) > 0);

    wchar_t tempAppData[MAX_PATH];
    _snwprintf_s(tempAppData, MAX_PATH, _TRUNCATE,
                 L"%lslunar-pinstore-test-%lu",
                 tempRoot, (unsigned long)GetCurrentProcessId());
    RemoveDirectoryW(tempAppData);
    CHECK(CreateDirectoryW(tempAppData, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(SetEnvironmentVariableW(L"APPDATA", tempAppData));
    PinStore_TestReset();

    uint8_t spki[32];
    for (int i = 0; i < 32; i++) spki[i] = (uint8_t)(i + 1);
    char hex[65];
    CertVerifyWin_Hex32(spki, hex);

    CHECK(PinStore_SavePin(PIN_ENDPOINT_DOH, "unit-doh", "resolver.example",
                           443, "unit-family", spki, hex,
                           "2026-01-01T00:00:00Z",
                           "2099-01-01T00:00:00Z",
                           1767225600LL, 4070908800LL,
                           "unit-test"));

    PinStore_TestReset();
    PinRecord rec;
    CHECK(PinStore_GetPin(PIN_ENDPOINT_DOH, "unit-doh", "resolver.example",
                          443, &rec));
    CHECK(rec.present);
    CHECK(memcmp(rec.spki, spki, sizeof spki) == 0);
    CHECK_EQ_STR(rec.spki_hex, hex);
    CHECK_EQ_STR(rec.not_after, "2099-01-01T00:00:00Z");
    CHECK_EQ_STR(rec.renewal_due, "2098-12-02T00:00:00Z");
    CHECK_EQ_STR(rec.last_status, "unit-test");
    CHECK_EQ_INT(PinStore_IsExpired(&rec), 0);
    CHECK_EQ_INT(PinStore_ShouldRenew(&rec), 0);
    // Single save => single-entry SPKI set whose newest member is the
    // record-level mirror.
    CHECK_EQ_INT(rec.spki_count, 1);
    CHECK(memcmp(rec.spkis[0].spki, spki, sizeof spki) == 0);
    CHECK_EQ_STR(rec.spkis[0].spki_hex, hex);

    wchar_t pinsPath[MAX_PATH];
    _snwprintf_s(pinsPath, MAX_PATH, _TRUNCATE, L"%ls\\Lunar\\pins.dat", tempAppData);
    CHECK(GetFileAttributesW(pinsPath) != INVALID_FILE_ATTRIBUTES);

    PinStore_TestReset();
    if (oldLen > 0 && oldLen < MAX_PATH) SetEnvironmentVariableW(L"APPDATA", oldAppData);
    else SetEnvironmentVariableW(L"APPDATA", NULL);
    DeleteFileW(pinsPath);
    wchar_t lunarDir[MAX_PATH];
    _snwprintf_s(lunarDir, MAX_PATH, _TRUNCATE, L"%ls\\Lunar", tempAppData);
    RemoveDirectoryW(lunarDir);
    RemoveDirectoryW(tempAppData);
}

// Shared scratch-dir setup for the multi-SPKI pin-store tests: redirect
// persistence via LUNAR_DATA_DIR (see app_paths.c) so nothing touches
// the real user profile.
static void pinstore_test_begin(wchar_t dir[MAX_PATH], const wchar_t *tag) {
    wchar_t tempRoot[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, tempRoot) > 0);
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%lslunar-pinstore-%ls-%lu",
                 tempRoot, tag, (unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(SetEnvironmentVariableW(L"LUNAR_DATA_DIR", dir));
    PinStore_TestReset();
}

static void pinstore_test_end(const wchar_t dir[MAX_PATH]) {
    PinStore_TestReset();
    SetEnvironmentVariableW(L"LUNAR_DATA_DIR", NULL);
    wchar_t pins[MAX_PATH];
    _snwprintf_s(pins, MAX_PATH, _TRUNCATE, L"%ls\\pins.dat", dir);
    DeleteFileW(pins);
    RemoveDirectoryW(dir);
}

static void test_pin_store_multi_spki(void) {
    wchar_t dir[MAX_PATH];
    pinstore_test_begin(dir, L"multi");

    // Six distinct keys; k[i] differs in the first and last byte.
    uint8_t k[6][32];
    char hex[6][65];
    for (int i = 0; i < 6; i++) {
        memset(k[i], 0, sizeof k[i]);
        k[i][0]  = (uint8_t)(0xA0 + i);
        k[i][31] = (uint8_t)(i + 1);
        CertVerifyWin_Hex32(k[i], hex[i]);
    }
    const int64_t NB = 1767225600LL;   // 2026-01-01
    const int64_t NA = 4070908800LL;   // 2099-01-01

#define SAVE_NTS(idx, nb, na, st) \
    CHECK(PinStore_SavePin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example", \
                           4460, "unit-family", k[idx], hex[idx],       \
                           "2026-01-01T00:00:00Z",                      \
                           "2099-01-01T00:00:00Z",                      \
                           (nb), (na), (st)))

    PinRecord rec;

    // First key: single-entry set.
    SAVE_NTS(0, NB, NA, "first-run-enrollment");
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, 1);
    CHECK(memcmp(rec.spki, k[0], 32) == 0);

    // Second key APPENDS (multi-POP observation); the mirror follows
    // the newest member, the older key is retained.
    SAVE_NTS(1, NB, NA, "pin-rotation");
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, 2);
    CHECK(memcmp(rec.spkis[0].spki, k[0], 32) == 0);
    CHECK(memcmp(rec.spkis[1].spki, k[1], 32) == 0);
    CHECK(memcmp(rec.spki, k[1], 32) == 0);

    // Re-saving a known key refreshes it in place (no duplicate) and
    // makes it the newest member again.
    SAVE_NTS(0, NB, NA, "scheduled-renewal");
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, 2);
    CHECK(memcmp(rec.spkis[0].spki, k[1], 32) == 0);
    CHECK(memcmp(rec.spkis[1].spki, k[0], 32) == 0);
    CHECK_EQ_STR(rec.last_status, "scheduled-renewal");

    // Fill to capacity, then overflow: the OLDEST member is evicted.
    SAVE_NTS(2, NB, NA, "pin-rotation");   // [k1 k0 k2]
    SAVE_NTS(3, NB, NA, "pin-rotation");   // [k1 k0 k2 k3]
    SAVE_NTS(4, NB, NA, "pin-rotation");   // k1 evicted -> [k0 k2 k3 k4]
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, PIN_STORE_MAX_SPKIS);
    CHECK(memcmp(rec.spkis[0].spki, k[0], 32) == 0);
    CHECK(memcmp(rec.spkis[1].spki, k[2], 32) == 0);
    CHECK(memcmp(rec.spkis[2].spki, k[3], 32) == 0);
    CHECK(memcmp(rec.spkis[3].spki, k[4], 32) == 0);
    CHECK(memcmp(rec.spki, k[4], 32) == 0);

    // All four un-expired keys are usable for TLS matching.
    uint8_t valid[PIN_STORE_MAX_SPKIS][32];
    CHECK_EQ_INT(PinStore_CollectValidSpkis(&rec, valid), 4);

    // An expired observation stays in the set (subject to eviction like
    // any member) but is excluded from the usable pin set.
    SAVE_NTS(5, NB, NB + 86400, "pin-rotation");  // long expired -> [k2 k3 k4 k5]
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, PIN_STORE_MAX_SPKIS);
    CHECK_EQ_INT(PinStore_CollectValidSpkis(&rec, valid), 3);
    CHECK(memcmp(valid[0], k[2], 32) == 0);
    CHECK(memcmp(valid[1], k[3], 32) == 0);
    CHECK(memcmp(valid[2], k[4], 32) == 0);
    // Record-level expiry keys on the newest member (k5, expired).
    CHECK_EQ_INT(PinStore_IsExpired(&rec), 1);

    // A different endpoint kind on the same host keeps its own set.
    CHECK(PinStore_SavePin(PIN_ENDPOINT_DOH, "unit-doh", "nts.example",
                           443, "unit-family", k[0], hex[0],
                           "2026-01-01T00:00:00Z", "2099-01-01T00:00:00Z",
                           NB, NA, "first-run-enrollment"));
    PinRecord doh;
    CHECK(PinStore_GetPin(PIN_ENDPOINT_DOH, "unit-doh", "nts.example",
                          443, &doh));
    CHECK_EQ_INT(doh.spki_count, 1);
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, PIN_STORE_MAX_SPKIS);

    // Reload from disk: set order, count and status survive the DPAPI
    // round trip (v2 file: one line per SPKI).
    PinStore_TestReset();
    memset(&rec, 0, sizeof rec);
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, PIN_STORE_MAX_SPKIS);
    CHECK(memcmp(rec.spkis[0].spki, k[2], 32) == 0);
    CHECK(memcmp(rec.spkis[1].spki, k[3], 32) == 0);
    CHECK(memcmp(rec.spkis[2].spki, k[4], 32) == 0);
    CHECK(memcmp(rec.spkis[3].spki, k[5], 32) == 0);
    CHECK_EQ_STR(rec.spkis[3].last_status, "pin-rotation");
    CHECK_EQ_INT(PinStore_CollectValidSpkis(&rec, valid), 3);

#undef SAVE_NTS
    pinstore_test_end(dir);
}

static void test_pin_store_v1_migration(void) {
    wchar_t dir[MAX_PATH];
    pinstore_test_begin(dir, L"miglegacy");

    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x40 + i);
    char hex[65];
    CertVerifyWin_Hex32(key, hex);

    // Hand-build a version-1 cache exactly as the previous release
    // serialized it (header "LUNAR_PINSTORE|1", one E-line per
    // endpoint) and protect it with the same DPAPI call.
    char text[512];
    _snprintf(text, sizeof text,
              "LUNAR_PINSTORE|1\n"
              "E|nts|unit-nts|nts.example|4460|unit-family|%s|"
              "2026-01-01T00:00:00Z|2099-01-01T00:00:00Z|"
              "1767225600|4070908800|4068316800|legacy-v1\n",
              hex);
    text[sizeof text - 1] = 0;

    DATA_BLOB in;
    in.pbData = (BYTE *)text;
    in.cbData = (DWORD)strlen(text);
    DATA_BLOB prot;
    memset(&prot, 0, sizeof prot);
    CHECK(CryptProtectData(&in, L"Lunar enrolled pin cache", NULL, NULL,
                           NULL, CRYPTPROTECT_UI_FORBIDDEN, &prot));

    wchar_t pins[MAX_PATH];
    _snwprintf_s(pins, MAX_PATH, _TRUNCATE, L"%ls\\pins.dat", dir);
    HANDLE h = CreateFileW(pins, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD wrote = 0;
    CHECK(WriteFile(h, prot.pbData, prot.cbData, &wrote, NULL) &&
          wrote == prot.cbData);
    CloseHandle(h);
    LocalFree(prot.pbData);

    // The v1 file must load WITHOUT error, as a single-entry SPKI set
    // with all metadata preserved.
    PinRecord rec;
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, 1);
    CHECK(memcmp(rec.spki, key, 32) == 0);
    CHECK_EQ_STR(rec.spki_hex, hex);
    CHECK_EQ_STR(rec.last_status, "legacy-v1");
    CHECK_EQ_INT(rec.renewal_due_unix, 4068316800LL);  // taken from the file

    // Appending a second key rewrites the cache as v2 in place; both
    // keys survive a reload.
    uint8_t key2[32];
    memset(key2, 0x77, sizeof key2);
    char hex2[65];
    CertVerifyWin_Hex32(key2, hex2);
    CHECK(PinStore_SavePin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                           4460, "unit-family", key2, hex2,
                           "2026-01-01T00:00:00Z", "2099-01-01T00:00:00Z",
                           1767225600LL, 4070908800LL, "pin-rotation"));
    PinStore_TestReset();
    memset(&rec, 0, sizeof rec);
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.spki_count, 2);
    CHECK(memcmp(rec.spkis[0].spki, key, 32) == 0);
    CHECK(memcmp(rec.spkis[1].spki, key2, 32) == 0);
    CHECK(memcmp(rec.spki, key2, 32) == 0);

    pinstore_test_end(dir);
}

static void test_pin_store_adaptive_margin(void) {
    const int64_t DAY = 24LL * 60 * 60;
    const int64_t CAP = 30 * DAY;
    const int64_t NB  = 1767225600LL;   // 2026-01-01

    // Long-lived leaves: fixed 30-day cap.
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 365 * DAY), CAP);
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 91 * DAY), CAP);
    // Exactly 90 days: validity/3 == cap.
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 90 * DAY), CAP);
    // Below the boundary the margin becomes proportional.
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 89 * DAY),
                 89 * DAY / 3);
    // CA/B short-lived profiles: 47-day and 6-day leaves.
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 47 * DAY),
                 47 * DAY / 3);
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB + 6 * DAY), 2 * DAY);
    // Unknown or garbled validity metadata: fall back to the cap.
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(0, NB + 90 * DAY), CAP);
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, 0), CAP);
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB, NB), CAP);
    CHECK_EQ_INT(PinStore_RenewalMarginSeconds(NB + DAY, NB), CAP);

    // End-to-end: a saved 6-day pin computes renewal_due = notAfter -
    // validity/3, i.e. it is NOT "always renewing" as the fixed 30-day
    // margin would make it.
    wchar_t dir[MAX_PATH];
    pinstore_test_begin(dir, L"margin");
    uint8_t key[32];
    memset(key, 0x21, sizeof key);
    char hex[65];
    CertVerifyWin_Hex32(key, hex);
    int64_t na = NB + 6 * DAY;
    CHECK(PinStore_SavePin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                           4460, "unit-family", key, hex,
                           "2026-01-01T00:00:00Z", "2026-01-07T00:00:00Z",
                           NB, na, "first-run-enrollment"));
    PinRecord rec;
    CHECK(PinStore_GetPin(PIN_ENDPOINT_NTS, "unit-nts", "nts.example",
                          4460, &rec));
    CHECK_EQ_INT(rec.renewal_due_unix, na - 2 * DAY);
    pinstore_test_end(dir);
}

static void test_dns_build_query(void) {
    // Known-answer for "example.com" / A / id=0x1234.
    uint8_t buf[512];
    size_t  len = 0;
    int     rc = Dns_BuildQueryA("example.com", 0x1234, buf, sizeof buf, &len);
    CHECK_EQ_INT(rc, 0);
    // Header is 12 bytes; body = 7 (example) + 3 (com) + 2 NULs + 4 (type+class).
    //   12 + 1 + 7 + 1 + 3 + 1 + 2 + 2 = 29
    CHECK_EQ_INT(len, 29);
    CHECK_EQ_INT(buf[0], 0x12);
    CHECK_EQ_INT(buf[1], 0x34);
    // QR=0, OPCODE=0, RD=1  -> flags high byte = 0x01; low byte = 0.
    CHECK_EQ_INT(buf[2], 0x01);
    CHECK_EQ_INT(buf[3], 0x00);
    // QDCOUNT = 1, ANCOUNT = ARCOUNT = NSCOUNT = 0.
    CHECK_EQ_INT(buf[4], 0); CHECK_EQ_INT(buf[5], 1);
    CHECK_EQ_INT(buf[6], 0); CHECK_EQ_INT(buf[7], 0);
    CHECK_EQ_INT(buf[8], 0); CHECK_EQ_INT(buf[9], 0);
    CHECK_EQ_INT(buf[10], 0); CHECK_EQ_INT(buf[11], 0);
    // Qname: 7 "example" 3 "com" 0
    CHECK_EQ_INT(buf[12], 7);
    CHECK(memcmp(buf + 13, "example", 7) == 0);
    CHECK_EQ_INT(buf[20], 3);
    CHECK(memcmp(buf + 21, "com", 3) == 0);
    CHECK_EQ_INT(buf[24], 0);
    // QTYPE = A (1), QCLASS = IN (1).
    CHECK_EQ_INT(buf[25], 0); CHECK_EQ_INT(buf[26], 1);
    CHECK_EQ_INT(buf[27], 0); CHECK_EQ_INT(buf[28], 1);

    // Invalid inputs: empty label, oversized label, total-too-long.
    CHECK_EQ_INT(Dns_BuildQueryA("",            0, buf, sizeof buf, &len), -1);
    CHECK_EQ_INT(Dns_BuildQueryA(".",           0, buf, sizeof buf, &len), -1);
    CHECK_EQ_INT(Dns_BuildQueryA("a..b",        0, buf, sizeof buf, &len), -1);
    char big[300]; memset(big, 'a', sizeof big); big[sizeof big - 1] = 0;
    CHECK_EQ_INT(Dns_BuildQueryA(big, 0, buf, sizeof buf, &len), -1);
    // Tiny out buffer.
    CHECK_EQ_INT(Dns_BuildQueryA("example.com", 0, buf, 10, &len), -1);
}

static void test_dns_parse_response(void) {
    // Hand-built response: id=0xabcd, QR=1, RD=1, RA=1, QDCOUNT=1,
    //   ANCOUNT=2, one A record 93.184.216.34 (TTL 300) + one A
    //   record 10.0.0.1 (TTL 120). Question uses the wire form, and
    //   each answer uses a 0xC00C compression pointer back to the
    //   qname.
    uint8_t r[512];
    size_t  pos = 0;
    r[pos++] = 0xab; r[pos++] = 0xcd;        // id
    r[pos++] = 0x81; r[pos++] = 0x80;        // QR=1 RD=1 RA=1 RCODE=0
    r[pos++] = 0x00; r[pos++] = 0x01;        // QDCOUNT
    r[pos++] = 0x00; r[pos++] = 0x02;        // ANCOUNT
    r[pos++] = 0x00; r[pos++] = 0x00;        // NSCOUNT
    r[pos++] = 0x00; r[pos++] = 0x00;        // ARCOUNT
    // qname: example.com
    r[pos++] = 7; memcpy(r + pos, "example", 7); pos += 7;
    r[pos++] = 3; memcpy(r + pos, "com",     3); pos += 3;
    r[pos++] = 0;
    r[pos++] = 0; r[pos++] = 1;              // type A
    r[pos++] = 0; r[pos++] = 1;              // class IN
    // Answer 1: name=pointer to 0x000C, type A, class IN, TTL 300, rdlen 4, rdata
    r[pos++] = 0xc0; r[pos++] = 0x0c;
    r[pos++] = 0; r[pos++] = 1;
    r[pos++] = 0; r[pos++] = 1;
    r[pos++] = 0; r[pos++] = 0; r[pos++] = 0x01; r[pos++] = 0x2c;  // TTL 300
    r[pos++] = 0; r[pos++] = 4;
    r[pos++] = 93; r[pos++] = 184; r[pos++] = 216; r[pos++] = 34;
    // Answer 2: same name ptr, A, IN, TTL 120, rdata 10.0.0.1
    r[pos++] = 0xc0; r[pos++] = 0x0c;
    r[pos++] = 0; r[pos++] = 1;
    r[pos++] = 0; r[pos++] = 1;
    r[pos++] = 0; r[pos++] = 0; r[pos++] = 0; r[pos++] = 120;
    r[pos++] = 0; r[pos++] = 4;
    r[pos++] = 10; r[pos++] = 0; r[pos++] = 0; r[pos++] = 1;

    char ips[8][16];
    size_t n_ips = 0;
    uint32_t min_ttl = 0;

    // Good path.
    CHECK_EQ_INT(Dns_ParseResponseA(r, pos, 0xabcd, "example.com",
                                    ips, 8, &n_ips, &min_ttl), 0);
    CHECK_EQ_INT(n_ips, 2);
    CHECK_EQ_STR(ips[0], "93.184.216.34");
    CHECK_EQ_STR(ips[1], "10.0.0.1");
    CHECK_EQ_INT(min_ttl, 120);

    // QID mismatch.
    CHECK_EQ_INT(Dns_ParseResponseA(r, pos, 0x0000, "example.com",
                                    ips, 8, &n_ips, &min_ttl), -3);
    // Host mismatch (case-insensitive OK, "EXAMPLE.COM" should still match).
    CHECK_EQ_INT(Dns_ParseResponseA(r, pos, 0xabcd, "EXAMPLE.COM",
                                    ips, 8, &n_ips, &min_ttl), 0);
    // Host mismatch (real).
    CHECK_EQ_INT(Dns_ParseResponseA(r, pos, 0xabcd, "other.com",
                                    ips, 8, &n_ips, &min_ttl), -3);
    // Truncated.
    CHECK_EQ_INT(Dns_ParseResponseA(r, 11, 0xabcd, "example.com",
                                    ips, 8, &n_ips, &min_ttl), -1);

    // RCODE != 0.
    uint8_t r_rcode[64];
    memcpy(r_rcode, r, pos);
    r_rcode[3] = 0x83;     // RCODE = 3 (NXDOMAIN), QR=1 RD=1 RA=1
    CHECK_EQ_INT(Dns_ParseResponseA(r_rcode, pos, 0xabcd, "example.com",
                                    ips, 8, &n_ips, &min_ttl), -2);
}

// Net_ParseIp: IPv4 and IPv6 literals -> the right family + sockaddr len.
static void test_net_parseip(void) {
    struct sockaddr_storage ss;
    int len = 0;

    CHECK_EQ_INT(Net_ParseIp("93.184.216.34", 123, &ss, &len), AF_INET);
    CHECK_EQ_INT(len, (int)sizeof(struct sockaddr_in));
    CHECK_EQ_INT(((struct sockaddr_in *)&ss)->sin_port, htons(123));

    CHECK_EQ_INT(Net_ParseIp("2606:4700:4700::1111", 4460, &ss, &len),
                 AF_INET6);
    CHECK_EQ_INT(len, (int)sizeof(struct sockaddr_in6));
    CHECK_EQ_INT(((struct sockaddr_in6 *)&ss)->sin6_port, htons(4460));

    // Compressed and full v6 forms both parse.
    CHECK_EQ_INT(Net_ParseIp("::1", 1, &ss, &len), AF_INET6);
    CHECK_EQ_INT(Net_ParseIp("2001:db8:0:0:0:0:0:1", 1, &ss, &len), AF_INET6);

    // Garbage -> AF_UNSPEC.
    CHECK_EQ_INT(Net_ParseIp("not-an-ip", 1, &ss, &len), AF_UNSPEC);
    CHECK_EQ_INT(Net_ParseIp("999.1.1.1", 1, &ss, &len), AF_UNSPEC);
}

// Dns_ParseResponse for AAAA: a crafted response yields the formatted
// IPv6 string, and the general parser agrees with the A-only one on A.
static void test_dns_parse_aaaa(void) {
    // id=0x1234, QR/RD/RA, QDCOUNT=1, ANCOUNT=1; question example.com/AAAA;
    // one AAAA record 2001:db8::1 (TTL 200) via a 0xC00C name pointer.
    uint8_t r[512];
    size_t  pos = 0;
    r[pos++] = 0x12; r[pos++] = 0x34;
    r[pos++] = 0x81; r[pos++] = 0x80;
    r[pos++] = 0x00; r[pos++] = 0x01;
    r[pos++] = 0x00; r[pos++] = 0x01;
    r[pos++] = 0x00; r[pos++] = 0x00;
    r[pos++] = 0x00; r[pos++] = 0x00;
    r[pos++] = 7; memcpy(r + pos, "example", 7); pos += 7;
    r[pos++] = 3; memcpy(r + pos, "com",     3); pos += 3;
    r[pos++] = 0;
    r[pos++] = 0; r[pos++] = 28;              // type AAAA
    r[pos++] = 0; r[pos++] = 1;               // class IN
    r[pos++] = 0xc0; r[pos++] = 0x0c;         // name ptr
    r[pos++] = 0; r[pos++] = 28;              // AAAA
    r[pos++] = 0; r[pos++] = 1;               // IN
    r[pos++] = 0; r[pos++] = 0; r[pos++] = 0; r[pos++] = 200;  // TTL
    r[pos++] = 0; r[pos++] = 16;              // rdlen
    // 2001:0db8:0000:...:0001
    uint8_t v6[16] = { 0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1 };
    memcpy(r + pos, v6, 16); pos += 16;

    char ips[8][46];
    size_t n = 0;
    uint32_t ttl = 0;
    CHECK_EQ_INT(Dns_ParseResponse(r, pos, 0x1234, "example.com",
                                   28 /*AAAA*/, ips, 8, &n, &ttl), 0);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_STR(ips[0], "2001:db8::1");
    CHECK_EQ_INT(ttl, 200);

    // Asking for A against a response to an AAAA question is a question
    // mismatch (-1): the parser insists the question type matches.
    n = 0;
    CHECK_EQ_INT(Dns_ParseResponse(r, pos, 0x1234, "example.com",
                                   1 /*A*/, ips, 8, &n, &ttl), -1);
}

static void test_dns_intersect(void) {
    char a[4][16] = { "1.2.3.4", "5.6.7.8", "9.10.11.12", "" };
    char b[4][16] = { "99.0.0.1", "5.6.7.8", "1.2.3.4", "" };
    char out[16] = {0};

    // First overlap (in a's order) is 1.2.3.4.
    CHECK_EQ_INT(Dns_Intersect(a, 3, b, 3, out), 0);
    CHECK_EQ_STR(out, "1.2.3.4");

    // Disjoint.
    char c[2][16] = { "8.8.8.8", "9.9.9.9" };
    char d[2][16] = { "1.1.1.1", "2.2.2.2" };
    CHECK_EQ_INT(Dns_Intersect(c, 2, d, 2, out), -1);

    // Empty sets: both orderings return -1.
    CHECK_EQ_INT(Dns_Intersect(a, 0, b, 3, out), -1);
    CHECK_EQ_INT(Dns_Intersect(a, 3, b, 0, out), -1);
    CHECK_EQ_INT(Dns_Intersect(a, 0, b, 0, out), -1);
}

static void test_dns_cache_clear(void) {
    // Just make sure the API is callable and idempotent. Actual cache
    // hit/miss behaviour is exercised end-to-end in live validation,
    // since the cache state is internal.
    Dns_CacheClear();
    Dns_CacheClear();
    CHECK(1);
}

static void test_app_data_path(void) {
    wchar_t dir[MAX_PATH] = { 0 };
    wchar_t file[MAX_PATH] = { 0 };
    wchar_t tiny[4] = { L'x', 0 };

    CHECK_EQ_INT(Lunar_AppDataPathW(NULL, MAX_PATH, L"x.dat"), 0);
    CHECK_EQ_INT(Lunar_AppDataPathW(tiny, 0, L"x.dat"), 0);

    CHECK_EQ_INT(Lunar_AppDataPathW(dir, MAX_PATH, NULL), 1);
    CHECK(dir[0] != 0);
    CHECK(wcsstr(dir, L"\\Lunar") != NULL);

    CHECK_EQ_INT(Lunar_AppDataPathW(file, MAX_PATH, L"unit-test.dat"), 1);
    CHECK(wcsstr(file, L"\\Lunar\\unit-test.dat") != NULL);

    CHECK_EQ_INT(Lunar_AppDataPathW(tiny, 4, L"unit-test.dat"), 0);
    CHECK_EQ_INT(tiny[0], 0);
}

// ---------------------------------------------------------------------------
// Interval-only trust: measured anchor error, widen-only witness,
// tick-vs-QPC continuity trip.
// ---------------------------------------------------------------------------

static void test_interval_only_trust(void) {
    Clock_Shutdown();
    Clock_Init();

    // Anchor with a MEASURED 45 ms base: the published interval must start
    // from the measurement, not from the legacy flat 200 ms.
    int64_t utc = 1700000000000LL;
    int64_t qpc = Clock_Qpc();
    Clock_OnPollCycle(TRUST_OK, utc, qpc, 45);
    ClockDisplay d;
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_OK);
    CHECK(d.boundMs >= 45 && d.boundMs < 65);

    // Widen-only witness: a DISAGREEING unauthenticated cluster inflates
    // the interval and drops the tier to HOLDOVER...
    Clock_OnCoreWitness(utc + 5000, qpc);
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_HOLDOVER);
    CHECK(d.boundMs >= 5000);
    // ...and an AGREEING witness must NOT clear the inflate or restore
    // the tier (unauthenticated evidence never tightens a claim).
    Clock_OnCoreWitness(utc, qpc);
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_HOLDOVER);
    CHECK(d.boundMs >= 5000);
    // Only a fresh authenticated cycle clears it -- back to the measured
    // base, not the default.
    Clock_OnPollCycle(TRUST_OK, utc, Clock_Qpc(), 45);
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_OK);
    CHECK(d.boundMs < 200);

    // Continuity trip: wall time (tick64) advances while QPC stands still
    // -- an unreported suspend. The cross-check must trip to REACQUIRING
    // and withdraw the running time entirely.
    Clock_TestAgeTickOnly(30000);
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_REACQUIRING);
    int64_t ms = 0;
    CHECK_EQ_INT(Clock_NowUtcMs(&ms), 0);
    // A full authenticated cycle re-anchors out of it.
    Clock_OnPollCycle(TRUST_OK, utc + 120000, Clock_Qpc(), 45);
    Clock_GetDisplay(&d);
    CHECK_EQ_INT(d.state, TRUST_OK);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    test_tz_lookup();
    test_ntp_timestamp_math();
    test_ntp_header_validation();
    test_clock_discipline();
    test_clock_display_states();
    test_clock_drift_convergence();
    test_clock_rate_clamps();
    test_clock_jitter_rejection();
    test_clock_fault_gate_and_escape();
    test_clock_short_interval_guard();
    test_clock_resume_consistent();
    test_interval_only_trust();
    test_ntp_concur();
    test_ntp_concur_rotated();
    test_ntp_kiss_of_death();
    test_tz_winmap();
    test_siv_rfc5297_appendix_a1();
    test_siv_rfc5297_appendix_a2();
    test_siv_edge_cases();
    test_ntske_client_request();
    test_ntske_parse_valid();
    test_ntske_parse_errors();
    test_nts_ef_roundtrip();
    test_nts_ef_tamper();
    test_update_version_cmp();
    test_nts_cookie_jar();
    test_nts_pool_pins();
    test_logbuf_basic();
    test_logbuf_truncation();
    test_logbuf_collect_since();
    test_logbuf_collect_wrapped();
    test_doh_rotation_corroboration();
    test_doh_failure_backoff();
    test_nts_picker_prefers_near_providers();
    test_hpack_integers();
    test_hpack_huffman_table();
    test_hpack_huffman_strings();
    test_hpack_decode_blocks();
    test_h2_post_once();
    test_clock_rate_clamp_scales_with_interval();

    test_tz_bounds();
    test_tz_southern_hemisphere();
    test_tz_half_hour_zones();
    test_tz_all_zones_smoke();
    test_tzif_malformed();
    test_breakdown_leap_years();
    test_posix_footer_hardening();

    test_dns_pool_pins();
    test_dns_pick_resolvers();
    test_dns_build_query();
    test_dns_parse_response();
    test_net_parseip();
    test_dns_parse_aaaa();
    test_dns_intersect();
    test_dns_cache_clear();
    test_pin_store_roundtrip();
    test_pin_store_multi_spki();
    test_pin_store_v1_migration();
    test_pin_store_adaptive_margin();
    test_app_data_path();

    printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
