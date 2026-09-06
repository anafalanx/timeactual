/* lunarx.c -- the Tcl command surface over Lunar's C engine.
 *
 * A stubs-based Tcl extension (statically linked into the exe and registered
 * in Lunar_AppInit) that exposes the kept engine -- clock discipline, the
 * NTP/NTS aggregator, the update check -- to the Tcl/Tk shell as ::lunar::*
 * commands. The UI polls these (all quick, mutex-protected reads); the
 * networking stays on the engine's own background threads.
 *
 * Follows els's extension pattern: Tcl_InitStubs, fully-qualified command
 * names created at Init, "" as the success sentinel.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>   /* SetWindowSubclass (resize + WM_ENDSESSION hook) */
#include <mmsystem.h>   /* PlaySoundW (chime) */
#include <tcl.h>

#include <string.h>
#include <stdint.h>
#include <math.h>

#include "clock.h"
#include "ntp.h"
#include "update_check.h"
#include "tz.h"
#include "tz_winmap.h"
#include "logbuf.h"
#include "version.h"

extern int LunarClock_Init(Tcl_Interp *interp);

static const char *trust_name(TrustState s) {
    switch (s) {
        case TRUST_OK:          return "ok";
        case TRUST_HOLDOVER:    return "holdover";
        case TRUST_REACQUIRING: return "reacquiring";
        case TRUST_INOP:        default: return "inop";
    }
}

static const char *auth_name(NtpAuthMode m) {
    switch (m) {
        case NTP_AUTH_PLAIN_SNTP:   return "sntp";
        case NTP_AUTH_ENROLLED_PIN: return "nts";
        case NTP_AUTH_ROTATED_PIN:  return "nts-rotated";
        case NTP_AUTH_NONE:         default: return "none";
    }
}

#define PUT(d, k, v) Tcl_DictObjPut(ip, (d), Tcl_NewStringObj((k), -1), (v))

/* ---- chime -------------------------------------------------------------
 * An 880 Hz / 250 ms sine chime with a 200 ms linear attack and a hard end,
 * volume-compensated against the system master slider and played from an
 * in-memory WAV.
 *
 * Reliability (why this is more than a PlaySound one-liner): Windows parks
 * an idle render endpoint in device power state D3. The first sound after
 * that idle forces a D3->D0 wake whose documented exit latency is up to
 * 35-300 ms (PortCls IAdapterPowerManagement3::D3ExitLatencyChanged), during
 * which the codec/amp are still ramping and the mixer drops the leading
 * samples -- so the tone's onset is clipped to a click. (This is exactly why
 * the very first mark-chime clicked but every Test press right after played
 * cleanly: the endpoint was warm by then.) To render the tone into an awake,
 * settled endpoint we prepend ~300 ms of digital silence: the wake/settle
 * lands on the silence, and the 880 Hz section starts only once the device
 * is streaming. The play runs SND_SYNC on a short-lived worker thread from a
 * PRIVATE heap buffer (so the buffer is valid for the whole play and
 * concurrent chimes can't corrupt it), gated by a busy flag so an
 * overlapping request is dropped rather than cancelling the one in flight
 * (PlaySound is process-global-single-sound: a second play would otherwise
 * truncate the first). SND_NODEFAULT keeps a cold-open failure from
 * substituting the Windows 'ding'. */
extern float Sysvol_Get(void);   /* sysvol.c (compiled into the engine) */

#define BEEP_SAMPLE_RATE 44100
#define BEEP_PREWARM_MS  300                              /* covers the worst-case D3->D0 wake */
#define BEEP_TONE_MS     250
#define BEEP_ATTACK_MS   200
#define BEEP_PREWARM_FRAMES (BEEP_SAMPLE_RATE * BEEP_PREWARM_MS / 1000)
#define BEEP_TONE_FRAMES    (BEEP_SAMPLE_RATE * BEEP_TONE_MS / 1000)
#define BEEP_ATTACK_FRAMES  (BEEP_SAMPLE_RATE * BEEP_ATTACK_MS / 1000)
#define BEEP_TOTAL_FRAMES   (BEEP_PREWARM_FRAMES + BEEP_TONE_FRAMES)
#define BEEP_FREQ_HZ     880.0f                           /* A5 */
#define BEEP_TARGET_SPEAKER_AMPLITUDE 0.276f
#define BEEP_DATA_BYTES  (BEEP_TOTAL_FRAMES * 2)
#define BEEP_BUF_BYTES   (44 + BEEP_DATA_BYTES)
static_assert(BEEP_ATTACK_FRAMES > 0 && BEEP_ATTACK_FRAMES < BEEP_TONE_FRAMES,
              "chime attack must fit inside the tone");

static void WriteLE16(unsigned char *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void WriteLE32(unsigned char *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}
static void BuildWav(unsigned char *buf, float freqHz, float amplitude) {
    unsigned char *p = buf;
    memcpy(p, "RIFF", 4);               p += 4;
    WriteLE32(p, 36 + BEEP_DATA_BYTES); p += 4;
    memcpy(p, "WAVE", 4);               p += 4;
    memcpy(p, "fmt ", 4);               p += 4;
    WriteLE32(p, 16);                   p += 4;
    WriteLE16(p, 1);                    p += 2;
    WriteLE16(p, 1);                    p += 2;
    WriteLE32(p, BEEP_SAMPLE_RATE);     p += 4;
    WriteLE32(p, BEEP_SAMPLE_RATE * 2); p += 4;
    WriteLE16(p, 2);                    p += 2;
    WriteLE16(p, 16);                   p += 2;
    memcpy(p, "data", 4);               p += 4;
    WriteLE32(p, BEEP_DATA_BYTES);      p += 4;
    /* Leading silence warms a D3-parked endpoint before the tone renders. */
    for (int i = 0; i < BEEP_PREWARM_FRAMES; i++) { WriteLE16(p, 0); p += 2; }
    const float TAU = 6.28318530717958647692f;
    for (int i = 0; i < BEEP_TONE_FRAMES; i++) {
        /* Rise linearly for 200 ms, then remain at full amplitude. Deliberately
         * do not apply a release envelope: the tone stops at its fixed end. */
        float env = i < BEEP_ATTACK_FRAMES
                  ? (float)i / (float)BEEP_ATTACK_FRAMES
                  : 1.0f;
        float s = sinf(TAU * freqHz * (float)i / BEEP_SAMPLE_RATE) * env * amplitude;
        int v = (int)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        WriteLE16(p, (uint16_t)(int16_t)v);
        p += 2;
    }
}

static volatile LONG g_beepBusy   = 0;      /* 1 while a chime is playing */
static HANDLE        g_beepThread  = nullptr; /* current worker (UI-thread only) */

static DWORD WINAPI BeepThread([[maybe_unused]] LPVOID arg) {
    /* Read the volume here (not on the UI thread) so the amplitude reflects
     * the endpoint at render time and the COM/audio touch doesn't jitter
     * the UI. Sysvol_Get returns 0.0 when muted; skip in that case. */
    float v = Sysvol_Get();
    if (v > 0.01f) {
        float amp = BEEP_TARGET_SPEAKER_AMPLITUDE / v;
        if (amp > 0.90f) amp = 0.90f;
        if (amp < 0.05f) amp = 0.05f;
        unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, BEEP_BUF_BYTES);
        if (buf) {
            BuildWav(buf, BEEP_FREQ_HZ, amp);
            PlaySoundW((LPCWSTR)buf, NULL, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    InterlockedExchange(&g_beepBusy, 0);
    return 0;
}

static void PlayBeepImpl(void) {
    /* Drop an overlapping request instead of queuing or truncating: if a
     * chime is already in flight (rapid Test clicks, or a Test press during
     * a mark chime), ignore the new one. */
    if (InterlockedCompareExchange(&g_beepBusy, 1, 0) != 0) return;
    /* busy just went 0->1, so any previous worker has finished; reap it. */
    if (g_beepThread) { CloseHandle(g_beepThread); g_beepThread = nullptr; }
    HANDLE t = CreateThread(nullptr, 0, BeepThread, nullptr, 0, nullptr);
    if (!t) { InterlockedExchange(&g_beepBusy, 0); return; }
    g_beepThread = t;
}

/* Stop any in-flight chime and reap its worker. Called on the shutdown
 * paths so process teardown cannot race the play thread freeing its
 * buffer. UI-thread only (same as PlayBeepImpl), so g_beepThread is safe. */
static void Beep_Shutdown(void) {
    PlaySoundW(nullptr, nullptr, 0);   /* abort any in-flight SND_SYNC play */
    HANDLE t = g_beepThread;
    if (t) { WaitForSingleObject(t, 1500); CloseHandle(t); g_beepThread = nullptr; }
    InterlockedExchange(&g_beepBusy, 0);
}

/* lunar::beep -- play the chime once. */
static int Beep_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                    int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    PlayBeepImpl();
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* ---- just-in-time pre-warm --------------------------------------------
 * ::lunar::prewarm plays ~300 ms of silence to bring a D3-parked render
 * endpoint back to D0 *ahead* of a due chime. lunar.tcl calls it ~3 s
 * before an armed mark, so the mark-chime then renders into an already-
 * awake, settled endpoint -- no wake clip, and the amp's power-on transient
 * happens here (during silence) rather than on the tone. Between marks it
 * does nothing, so unlike a continuous keep-alive stream it never holds the
 * endpoint (or downstream HDMI/BT/USB gear) awake or costs steady-state
 * power. SND_ASYNC keeps it off the UI's critical path; it is skipped while
 * a chime is in flight (the endpoint is warm then, and an async play would
 * otherwise cut the chime short -- PlaySound is process-global-single). */
static unsigned char g_silence[44 + (BEEP_PREWARM_FRAMES * 2)];
static int           g_silence_ready = 0;   /* UI-thread-only, like PlayBeepImpl */

static void PrewarmImpl(void) {
    if (g_beepBusy) return;
    if (!g_silence_ready) {
        unsigned char *p = g_silence;
        uint32_t data = BEEP_PREWARM_FRAMES * 2;
        memcpy(p, "RIFF", 4);               p += 4;
        WriteLE32(p, 36 + data);            p += 4;
        memcpy(p, "WAVE", 4);               p += 4;
        memcpy(p, "fmt ", 4);               p += 4;
        WriteLE32(p, 16);                   p += 4;
        WriteLE16(p, 1);                    p += 2;
        WriteLE16(p, 1);                    p += 2;
        WriteLE32(p, BEEP_SAMPLE_RATE);     p += 4;
        WriteLE32(p, BEEP_SAMPLE_RATE * 2); p += 4;
        WriteLE16(p, 2);                    p += 2;
        WriteLE16(p, 16);                   p += 2;
        memcpy(p, "data", 4);               p += 4;
        WriteLE32(p, data);                 p += 4;
        /* the BEEP_PREWARM_FRAMES sample frames stay zero (static storage) */
        g_silence_ready = 1;
    }
    PlaySoundW((LPCWSTR)g_silence, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* lunar::prewarm -- wake the audio endpoint ahead of a due chime. */
static int Prewarm_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    PrewarmImpl();
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::engine_start -- one-time engine bootstrap. */
static int EngineStart_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                           int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);   /* refcounted; safe if engine also inits */
        Clock_Init();
        UpdateCheck_Start();
        Ntp_Start();
    }
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::syncnow -- kick a polling cycle (no-op if one is in flight). */
static int SyncNow_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Ntp_Start();
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::shutdown -- drain workers, persist rate + the diagnostic log
 * before the process exits (crash-survival parity with the Win32 shell). */
static int Shutdown_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                        int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Beep_Shutdown();
    Ntp_Shutdown();
    Clock_Shutdown();
    Log_FlushToDisk(NULL);
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::log_text -- the whole in-memory event log, oldest first, as one
 * \r\n-delimited string (what the Log viewer shows). */
static int LogText_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    size_t need = Log_Snapshot(NULL, 0);
    char *buf = (char *)Tcl_Alloc(need + 1);
    size_t n = Log_Snapshot(buf, need + 1);
    Tcl_SetObjResult(ip, Tcl_NewStringObj(buf, (Tcl_Size)n));
    Tcl_Free(buf);
    return TCL_OK;
}

/* lunar::log_events ?sinceSeq? -- structured incremental read of the
 * in-memory event ring for the Tcl-side persistent event store: a list of
 * dicts {seq ageMs utcMs trusted msg}, oldest first, holding every ring
 * entry with seq > sinceSeq. ageMs is the entry's age on the monotonic
 * clock at read time, so the caller can approximate an absolute stamp for
 * untrusted entries (system now - ageMs) without trusting the ring's
 * wall-clock state. */
static int LogEvents_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    Tcl_WideInt since = 0;
    if (objc > 2) { Tcl_WrongNumArgs(ip, 1, objv, "?sinceSeq?"); return TCL_ERROR; }
    if (objc == 2 && Tcl_GetWideIntFromObj(ip, objv[1], &since) != TCL_OK) {
        return TCL_ERROR;
    }
    if (since < 0) since = 0;

    LogRawEntry *buf =
        (LogRawEntry *)Tcl_Alloc(sizeof(LogRawEntry) * LOGBUF_CAP);
    size_t n = Log_CollectSince((uint64_t)since, buf, LOGBUF_CAP);
    uint64_t nowTick = GetTickCount64();

    Tcl_Obj *list = Tcl_NewListObj(0, nullptr);
    for (size_t i = 0; i < n; i++) {
        const LogRawEntry *e = &buf[i];
        uint64_t age = (nowTick >= e->tickMs) ? (nowTick - e->tickMs) : 0;
        Tcl_Obj *d = Tcl_NewDictObj();
        PUT(d, "seq",     Tcl_NewWideIntObj((Tcl_WideInt)e->seq));
        PUT(d, "ageMs",   Tcl_NewWideIntObj((Tcl_WideInt)age));
        PUT(d, "utcMs",   Tcl_NewWideIntObj((Tcl_WideInt)e->utcMs));
        /* resolved here, matching Log_Snapshot's display rule, so the Tcl
         * side never needs a second copy of the trust convention */
        PUT(d, "trusted", Tcl_NewIntObj((e->trusted && e->utcMs != 0) ? 1 : 0));
        PUT(d, "msg",     Tcl_NewStringObj(e->msg, -1));
        Tcl_ListObjAppendElement(ip, list, d);
    }
    Tcl_Free((char *)buf);
    Tcl_SetObjResult(ip, list);
    return TCL_OK;
}

/* lunar::about -- {version X tzdata Y} for the About box. */
static int About_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                     int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "version", Tcl_NewStringObj(LUNAR_VERSION_STR, -1));
    PUT(d, "tzdata",  Tcl_NewStringObj(Tz_Version(), -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::status -- a dict of everything the dashboard needs. */
static int Status_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }

    ClockDisplay disp;
    Clock_GetDisplay(&disp);

    int64_t sysDelta = 0;
    int sysValid = Clock_SystemDeltaMs(&sysDelta);

    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "state",         Tcl_NewStringObj(trust_name(disp.state), -1));
    PUT(d, "hasTime",       Tcl_NewIntObj(disp.state >= TRUST_HOLDOVER ? 1 : 0));
    PUT(d, "utcMs",         Tcl_NewWideIntObj((Tcl_WideInt)disp.utcMs));
    PUT(d, "boundMs",       Tcl_NewWideIntObj((Tcl_WideInt)disp.boundMs));
    PUT(d, "lastSyncUtcMs", Tcl_NewWideIntObj((Tcl_WideInt)disp.lastSyncUtcMs));
    PUT(d, "lastSyncAgeMs", Tcl_NewWideIntObj((Tcl_WideInt)disp.lastSyncAgeMs));
    PUT(d, "sysDeltaValid", Tcl_NewIntObj(sysValid ? 1 : 0));
    PUT(d, "sysDeltaMs",    Tcl_NewWideIntObj((Tcl_WideInt)sysDelta));
    PUT(d, "ratePpm",       Tcl_NewIntObj((int)Clock_RatePpm()));
    PUT(d, "spreadMs",      Tcl_NewWideIntObj((Tcl_WideInt)Ntp_LastSpreadMs()));
    PUT(d, "ntsSpreadMs",   Tcl_NewWideIntObj((Tcl_WideInt)Ntp_LastNtsSpreadMs()));
    PUT(d, "synced",        Tcl_NewIntObj(Ntp_IsSynced() ? 1 : 0));

    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::sources -- a list of per-source dicts from the last cycle. */
static int Sources_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }

    NtpSourceResult res[NTP_SOURCE_COUNT];
    Ntp_GetResults(res);

    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        const NtpSourceResult *r = &res[i];
        Tcl_Obj *e = Tcl_NewDictObj();
        PUT(e, "slot",   Tcl_NewIntObj(i));
        PUT(e, "kind",   Tcl_NewStringObj(i >= NTP_FIRST_NTS_SLOT ? "nts" : "core", -1));
        PUT(e, "label",  Tcl_NewStringObj(r->label ? r->label : "", -1));
        PUT(e, "ok",     Tcl_NewIntObj(r->ok ? 1 : 0));
        PUT(e, "offsetMs", Tcl_NewWideIntObj((Tcl_WideInt)r->offsetMs));
        PUT(e, "rttMs",  Tcl_NewIntObj((int)r->rttMs));
        PUT(e, "auth",   Tcl_NewStringObj(auth_name(r->authMode), -1));
        PUT(e, "family", Tcl_NewStringObj(r->operatorFamily ? r->operatorFamily : "", -1));
        Tcl_ListObjAppendElement(ip, list, e);
    }
    Tcl_SetObjResult(ip, list);
    return TCL_OK;
}

/* One-entry TzId cache: the UI resolves the same zone name every tick.
 * Commands only ever run on the Tk thread, so no locking. */
static char g_tzCacheName[64];
static TzId g_tzCacheId = TZ_ID_INVALID;

static TzId tz_resolve(const char *name) {
    if (!name || !name[0]) return TZ_ID_INVALID;
    if (g_tzCacheId != TZ_ID_INVALID &&
        strcmp(name, g_tzCacheName) == 0) return g_tzCacheId;
    TzId id = Tz_FindByName(name);
    if (id != TZ_ID_INVALID && strlen(name) < sizeof g_tzCacheName) {
        strcpy(g_tzCacheName, name);
        g_tzCacheId = id;
    }
    return id;
}

/* lunar::localtime utcMs zone -- break a UTC instant into wall-clock
 * components for an embedded IANA zone (never the OS). Errors on a zone
 * missing from the embedded index so the caller can fall back. */
static int LocalTime_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 3) { Tcl_WrongNumArgs(ip, 1, objv, "utcMs zone"); return TCL_ERROR; }
    Tcl_WideInt utcMs;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &utcMs) != TCL_OK) return TCL_ERROR;
    const char *zone = Tcl_GetString(objv[2]);

    TzId id = tz_resolve(zone);
    if (id == TZ_ID_INVALID) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("unknown zone \"%s\"", zone));
        return TCL_ERROR;
    }
    TzifLocal lt;
    if (!Tz_LocalFromUtcMs(id, (int64_t)utcMs, &lt)) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("cannot resolve %s", zone));
        return TCL_ERROR;
    }
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "year",   Tcl_NewIntObj(lt.year));
    PUT(d, "month",  Tcl_NewIntObj(lt.month));
    PUT(d, "day",    Tcl_NewIntObj(lt.mday));
    PUT(d, "hour",   Tcl_NewIntObj(lt.hour));
    PUT(d, "minute", Tcl_NewIntObj(lt.minute));
    PUT(d, "second", Tcl_NewIntObj(lt.second));
    PUT(d, "wday",   Tcl_NewIntObj(lt.wday));
    PUT(d, "isDst",  Tcl_NewIntObj(lt.isDst));
    PUT(d, "offSec", Tcl_NewIntObj(lt.utcOffsetSec));
    PUT(d, "abbr",   Tcl_NewStringObj(lt.abbr, -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::tz_list -- every embedded canonical zone name. */
static int TzList_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    int n = Tz_Count();
    for (int i = 0; i < n; i++) {
        const char *name = Tz_AtIndex(i);
        if (name) Tcl_ListObjAppendElement(ip, list, Tcl_NewStringObj(name, -1));
    }
    Tcl_SetObjResult(ip, list);
    return TCL_OK;
}

/* lunar::tz_version -- the embedded tzdata release ("2026b"). */
static int TzVersion_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_SetObjResult(ip, Tcl_NewStringObj(Tz_Version(), -1));
    return TCL_OK;
}

/* lunar::tz_suggest -- the OS zone mapped to an embedded IANA name, or "".
 * Reading the zone NAME is not trusting the OS clock. */
static int TzSuggest_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    char sug[64] = "";
    if (!TzWinmap_CurrentIana(sug, sizeof sug)) sug[0] = 0;
    Tcl_SetObjResult(ip, Tcl_NewStringObj(sug, -1));
    return TCL_OK;
}

/* ---- native window events ------------------------------------------------
 * Native messages arrive in the Tk window's message dispatch (Tk pumps the
 * message queue on the Tcl thread), so -- exactly like els's windrop -- we
 * never eval Tcl inline: we Tcl_QueueEvent and run ::lunar::window_event at
 * a safe point in the event loop. The subclass carries the resize-gesture
 * boundaries (for the square-window dance) and the WM_ENDSESSION persist. */
#define LUNAR_SUBCLASS_UID 0x4C55       /* 'LU' */

static Tcl_Interp *g_uiInterp = NULL;   /* set at watch_resize, same thread */

typedef struct UiEvent { Tcl_Event ev; char kind[16]; } UiEvent;

static int UiEventProc(Tcl_Event *evPtr, [[maybe_unused]] int flags) {
    UiEvent *te = (UiEvent *)evPtr;
    if (g_uiInterp) {
        Tcl_Obj *cmd = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(cmd);
        Tcl_ListObjAppendElement(g_uiInterp, cmd, Tcl_NewStringObj("::lunar::window_event", -1));
        Tcl_ListObjAppendElement(g_uiInterp, cmd, Tcl_NewStringObj(te->kind, -1));
        if (Tcl_EvalObjEx(g_uiInterp, cmd, TCL_EVAL_GLOBAL) != TCL_OK) {
            Tcl_BackgroundException(g_uiInterp, TCL_ERROR);
        }
        Tcl_DecrRefCount(cmd);
    }
    return 1;
}
static void ui_queue(const char *kind) {
    UiEvent *te = (UiEvent *)Tcl_Alloc(sizeof(UiEvent));
    te->ev.proc = UiEventProc;
    te->ev.nextPtr = NULL;
    lstrcpynA(te->kind, kind, (int)sizeof te->kind);
    Tcl_QueueEvent((Tcl_Event *)te, TCL_QUEUE_TAIL);
}

static LRESULT CALLBACK WindowSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR uid, [[maybe_unused]] DWORD_PTR ref) {
    if (msg == WM_ENTERSIZEMOVE) ui_queue("resize-start");
    if (msg == WM_EXITSIZEMOVE) ui_queue("resize-end");
    if (msg == WM_ENDSESSION && wp) {
        /* OS shutdown/logoff bypasses the normal quit path (and must never
         * raise the close-confirmation dialog): persist the disciplined
         * rate + the diagnostic log NOW (we run on the Tk thread, so
         * calling the engine directly is safe). No Ntp_Shutdown -- its
         * worker drain could eat the seconds Windows grants us. */
        Beep_Shutdown();
        Clock_Shutdown();
        Log_FlushToDisk(NULL);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, WindowSubclass, uid);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
/* lunar::frame_metrics hwnd -- the frame insets needed to convert Tk's client
 * geometry to the visible Windows-frame geometry.  Tk owns the resize itself;
 * this command is deliberately read-only. */
static int FrameMetrics_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                            int objc, Tcl_Obj *const objv[]) {
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd"); return TCL_ERROR; }
    Tcl_WideInt h;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &h) != TCL_OK) return TCL_ERROR;
    HWND hwnd = (HWND)(intptr_t)h;
    if (!IsWindow(hwnd)) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("not a window", -1));
        return TCL_ERROR;
    }
    HWND frame = GetAncestor(hwnd, GA_ROOT);
    if (!frame) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("top-level window not found", -1));
        return TCL_ERROR;
    }
    RECT outer, client;
    POINT origin = { 0, 0 };
    if (!GetWindowRect(frame, &outer) || !GetClientRect(frame, &client) ||
        !ClientToScreen(frame, &origin)) return TCL_ERROR;
    int clientWidth = client.right - client.left;
    int clientHeight = client.bottom - client.top;
    Tcl_Obj *result = Tcl_NewDictObj();
    Tcl_DictObjPut(ip, result, Tcl_NewStringObj("extraWidth", -1),
                   Tcl_NewIntObj((outer.right - outer.left) - clientWidth));
    Tcl_DictObjPut(ip, result, Tcl_NewStringObj("extraHeight", -1),
                   Tcl_NewIntObj((outer.bottom - outer.top) - clientHeight));
    Tcl_DictObjPut(ip, result, Tcl_NewStringObj("left", -1),
                   Tcl_NewIntObj(origin.x - outer.left));
    Tcl_DictObjPut(ip, result, Tcl_NewStringObj("top", -1),
                   Tcl_NewIntObj(origin.y - outer.top));
    HMONITOR monitor = MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { .cbSize = sizeof monitorInfo };
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        RECT work = monitorInfo.rcWork;
        Tcl_DictObjPut(ip, result, Tcl_NewStringObj("workX", -1),
                       Tcl_NewIntObj(work.left));
        Tcl_DictObjPut(ip, result, Tcl_NewStringObj("workY", -1),
                       Tcl_NewIntObj(work.top));
        Tcl_DictObjPut(ip, result, Tcl_NewStringObj("workWidth", -1),
                       Tcl_NewIntObj(work.right - work.left));
        Tcl_DictObjPut(ip, result, Tcl_NewStringObj("workHeight", -1),
                       Tcl_NewIntObj(work.bottom - work.top));
    }
    Tcl_SetObjResult(ip, result);
    return TCL_OK;
}

/* lunar::watch_resize hwnd -- report the native resize-gesture boundary to
 * Tk without altering a Windows sizing message or its geometry. */
static int ResizeWatch_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                           int objc, Tcl_Obj *const objv[]) {
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd"); return TCL_ERROR; }
    Tcl_WideInt h;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &h) != TCL_OK) return TCL_ERROR;
    HWND hwnd = (HWND)(intptr_t)h;
    if (!IsWindow(hwnd)) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("not a window", -1));
        return TCL_ERROR;
    }
    HWND frame = GetAncestor(hwnd, GA_ROOT);
    if (!frame || !SetWindowSubclass(frame, WindowSubclass, LUNAR_SUBCLASS_UID, 0)) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("could not watch resize", -1));
        return TCL_ERROR;
    }
    g_uiInterp = ip;
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::run_at_startup ?0|1? -- query (no arg) or set the HKCU Run entry.
 * Returns 1/0 on query, "" on a successful set, or a Tcl error if the write
 * fails. The Run key is the source of truth. */
static int RunAtStartup_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                            int objc, Tcl_Obj *const objv[]) {
    static const wchar_t *kSub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t *kVal = L"TimeActual";
    static const wchar_t *kOld = L"Lunar";       /* the value name before 0.58 */
    if (objc == 1) {
        int on = 0, legacy = 0; HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSub, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, kVal, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) on = 1;
            else if (RegQueryValueExW(k, kOld, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) legacy = 1;
            RegCloseKey(k);
        }
        if (legacy) {
            /* A pre-rename entry points at an exe that no longer exists:
             * re-register under the new name at our own path, then drop it. */
            HKEY w;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kSub, 0, NULL, 0, KEY_SET_VALUE,
                                NULL, &w, NULL) == ERROR_SUCCESS) {
                wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    wchar_t q[MAX_PATH + 2];
                    int qn = wsprintfW(q, L"\"%s\"", exe);
                    if (RegSetValueExW(w, kVal, 0, REG_SZ, (const BYTE *)q,
                                       (DWORD)((qn + 1) * (int)sizeof(wchar_t))) == ERROR_SUCCESS) {
                        RegDeleteValueW(w, kOld);
                        on = 1;
                    }
                }
                RegCloseKey(w);
            }
        }
        Tcl_SetObjResult(ip, Tcl_NewIntObj(on));
        return TCL_OK;
    }
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "?0|1?"); return TCL_ERROR; }
    int enable;
    if (Tcl_GetBooleanFromObj(ip, objv[1], &enable) != TCL_OK) return TCL_ERROR;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSub, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &k, NULL) != ERROR_SUCCESS) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("cannot open Run key", -1));
        return TCL_ERROR;
    }
    LSTATUS rc = ERROR_SUCCESS;
    if (enable) {
        wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wchar_t q[MAX_PATH + 2];
            int qn = wsprintfW(q, L"\"%s\"", exe);
            rc = RegSetValueExW(k, kVal, 0, REG_SZ, (const BYTE *)q,
                                (DWORD)((qn + 1) * (int)sizeof(wchar_t)));
        } else {
            rc = ERROR_INSUFFICIENT_BUFFER;
        }
    } else {
        rc = RegDeleteValueW(k, kVal);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        RegDeleteValueW(k, kOld);
    }
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("cannot update Run key", -1));
        return TCL_ERROR;
    }
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::update -- {available 0|1 version X.Y.Z}. */
static int Update_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    char ver[32] = "";
    int avail = UpdateCheck_Available(ver, sizeof ver);
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "available", Tcl_NewIntObj(avail ? 1 : 0));
    PUT(d, "version",   Tcl_NewStringObj(ver, -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

int Lunarx_Init(Tcl_Interp *ip) {
    if (Tcl_InitStubs(ip, "9.0", 0) == nullptr) return TCL_ERROR;
    Tcl_CreateNamespace(ip, "::lunar", nullptr, nullptr);
    if (LunarClock_Init(ip) != TCL_OK) return TCL_ERROR;
    Tcl_CreateObjCommand(ip, "::lunar::engine_start", EngineStart_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::syncnow",      SyncNow_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::shutdown",     Shutdown_Cmd,    nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::status",       Status_Cmd,      nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::sources",      Sources_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::update_status", Update_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::localtime",    LocalTime_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_list",      TzList_Cmd,      nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_version",   TzVersion_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_suggest",   TzSuggest_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::frame_metrics", FrameMetrics_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::watch_resize", ResizeWatch_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::run_at_startup", RunAtStartup_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::log_text",     LogText_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::log_events",   LogEvents_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::about",        About_Cmd,       nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::beep",         Beep_Cmd,        nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::prewarm",      Prewarm_Cmd,     nullptr, nullptr);
    if (Tcl_PkgProvide(ip, "lunarx", "0.1") != TCL_OK) return TCL_ERROR;
    return TCL_OK;
}
