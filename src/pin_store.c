// pin_store.c -- DPAPI-protected local SPKI enrollment cache.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <accctrl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pin_store.h"
#include "app_paths.h"
#include "cert_verify_win.h"
#include "logbuf.h"
#include "clock.h"

// Version 2: multiple E-lines may share one endpoint key (kind, label,
// host, port); together they form that endpoint's SPKI set, ordered
// oldest -> newest in file order. Version 1 files (one E-line per
// endpoint) parse under the same rules as single-entry sets and are
// upgraded in place on the next persist.
#define PINSTORE_VERSION 2
#define PINSTORE_MAX_ENTRIES 64
#define PINSTORE_RENEWAL_MARGIN_CAP_SECONDS (30LL * 24LL * 60LL * 60LL)
// Once a pin is inside its renewal window, re-attempt the CA refresh at
// most this often. Without this, an endpoint whose upstream cert has not
// rotated yet (its recomputed nextCa stays in the past) would force a
// full CA re-validation + whole-store rewrite on EVERY connection --
// a storm that also starves the parallel NTS handshakes. One hour is far
// more than enough to notice a rotation while eliminating the storm.
#define PINSTORE_RENEWAL_RETRY_COOLDOWN_SECONDS (60LL * 60LL)

typedef struct {
    PinEndpointKind kind;
    char label[48];
    char hostname[128];
    uint16_t port;
    char operator_family[48];
    PinRecord rec;
} PinEntry;

static CRITICAL_SECTION g_cs;
static int g_cs_init;
static int g_loaded;
static PinEntry g_entries[PINSTORE_MAX_ENTRIES];
static size_t g_entry_count;

static void EnsureCs(void)
{
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = 1;
    }
}

static int64_t NowUnixSeconds(void)
{
    // Prefer the disciplined clock -- the whole point of Lunar is not
    // to trust the OS clock, and a badly-wrong system clock would
    // otherwise force-expire every pin (or never renew them). Falls
    // back to the system clock only before the first sync of a run,
    // when pin enrollment necessarily happens anyway.
    //
    // Note: Windows CA chain validation (cert_verify_win.c) still uses
    // the system clock internally for notBefore/notAfter -- that part
    // is outside our control -- so this hardens only the expiry/renewal
    // bookkeeping we own.
    int64_t discMs = 0;
    if (Clock_NowUtcMs(&discMs) && discMs > 0) {
        return discMs / 1000;
    }

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static const char *KindName(PinEndpointKind kind)
{
    return kind == PIN_ENDPOINT_DOH ? "doh" :
           kind == PIN_ENDPOINT_NTS ? "nts" : "?";
}

static PinEndpointKind ParseKind(const char *s)
{
    if (strcmp(s, "doh") == 0) return PIN_ENDPOINT_DOH;
    if (strcmp(s, "nts") == 0) return PIN_ENDPOINT_NTS;
    return 0;
}

static int SafeCopy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0 || !src) return 0;
    if (strchr(src, '|') || strchr(src, '\n') || strchr(src, '\r')) return 0;
    size_t n = strlen(src);
    if (n >= dst_len) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

static int HexVal(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int HexToBytes32(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64 || !out) return 0;
    for (int i = 0; i < 32; i++) {
        int hi = HexVal(hex[i * 2]);
        int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

int64_t PinStore_RenewalMarginSeconds(int64_t not_before_unix,
                                      int64_t not_after_unix)
{
    // Adaptive margin: min(30 days, validity / 3). With the CA/B trend
    // toward short-lived leaves (47-day, even 6-day), a fixed 30-day
    // margin degenerates to "always renewing"; a third of the validity
    // period keeps a proportional refresh runway instead. Unknown or
    // garbled validity metadata falls back to the fixed cap.
    if (not_before_unix <= 0 || not_after_unix <= 0 ||
        not_before_unix >= not_after_unix) {
        return PINSTORE_RENEWAL_MARGIN_CAP_SECONDS;
    }
    int64_t adaptive = (not_after_unix - not_before_unix) / 3;
    return adaptive < PINSTORE_RENEWAL_MARGIN_CAP_SECONDS
        ? adaptive : PINSTORE_RENEWAL_MARGIN_CAP_SECONDS;
}

static int64_t ComputeRenewalDue(int64_t not_before, int64_t not_after)
{
    if (not_after <= 0) return 0;
    int64_t due = not_after - PinStore_RenewalMarginSeconds(not_before,
                                                            not_after);
    if (due < 0) due = 0;
    return due;
}

// Human-readable record of which margin rule applied, for audit lines.
static void FormatMarginDecision(int64_t not_before, int64_t not_after,
                                 char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    int64_t margin = PinStore_RenewalMarginSeconds(not_before, not_after);
    if (margin < PINSTORE_RENEWAL_MARGIN_CAP_SECONDS) {
        _snprintf(out, out_len, "%llds=validity/3", (long long)margin);
    } else if (not_before > 0 && not_after > not_before) {
        _snprintf(out, out_len, "%llds=30d-cap", (long long)margin);
    } else {
        _snprintf(out, out_len, "%llds=30d-default(validity-unknown)",
                  (long long)margin);
    }
    out[out_len - 1] = 0;
}

static void FormatUnixUtc(int64_t unix_seconds, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = 0;
    if (unix_seconds <= 0) {
        _snprintf(out, out_len, "unknown");
        out[out_len - 1] = 0;
        return;
    }

    ULARGE_INTEGER u;
    u.QuadPart = (uint64_t)unix_seconds * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) {
        _snprintf(out, out_len, "unknown");
        out[out_len - 1] = 0;
        return;
    }

    _snprintf(out, out_len, "%04u-%02u-%02uT%02u:%02u:%02uZ",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    out[out_len - 1] = 0;
}

static void LogPinRecordLocked(const char *event, const PinEntry *e)
{
    if (!event || !e || !e->rec.present) return;
    char margin[64];
    FormatMarginDecision(e->rec.not_before_unix, e->rec.not_after_unix,
                         margin, sizeof margin);
    Log_Append("pinstore: %s %s:%s host=%s port=%u family=%s valid=%s..%s nextCa=%s nextCaUnix=%lld margin=%s status=%s pins=%u spki=%s",
               event,
               KindName(e->kind), e->label, e->hostname, (unsigned)e->port,
               e->operator_family,
               e->rec.not_before, e->rec.not_after,
               e->rec.renewal_due[0] ? e->rec.renewal_due : "unknown",
               (long long)e->rec.renewal_due_unix,
               margin,
               e->rec.last_status,
               (unsigned)e->rec.spki_count,
               e->rec.spki_hex);
}

// Insert one SPKI observation into a record's set. A key already in
// the set has its metadata refreshed and moves to the newest position;
// a new key appends, evicting the oldest member when the set is full.
// The record's single-pin mirror fields always track the newest member.
static void RecordUpsertSpkiLocked(PinRecord *rec, const PinSpki *s)
{
    size_t idx = rec->spki_count;
    for (size_t i = 0; i < rec->spki_count; i++) {
        if (memcmp(rec->spkis[i].spki, s->spki, 32) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < rec->spki_count) {
        // Known key: shift the tail down and re-append as newest.
        for (size_t i = idx; i + 1 < rec->spki_count; i++) {
            rec->spkis[i] = rec->spkis[i + 1];
        }
        rec->spkis[rec->spki_count - 1] = *s;
    } else if (rec->spki_count < PIN_STORE_MAX_SPKIS) {
        rec->spkis[rec->spki_count++] = *s;
    } else {
        // Full: evict the oldest observation.
        for (size_t i = 0; i + 1 < PIN_STORE_MAX_SPKIS; i++) {
            rec->spkis[i] = rec->spkis[i + 1];
        }
        rec->spkis[PIN_STORE_MAX_SPKIS - 1] = *s;
    }

    const PinSpki *newest = &rec->spkis[rec->spki_count - 1];
    rec->present = 1;
    memcpy(rec->spki, newest->spki, 32);
    memcpy(rec->spki_hex, newest->spki_hex, sizeof rec->spki_hex);
    memcpy(rec->not_before, newest->not_before, sizeof rec->not_before);
    memcpy(rec->not_after, newest->not_after, sizeof rec->not_after);
    rec->not_before_unix  = newest->not_before_unix;
    rec->not_after_unix   = newest->not_after_unix;
    rec->renewal_due_unix = newest->renewal_due_unix;
    memcpy(rec->renewal_due, newest->renewal_due, sizeof rec->renewal_due);
    memcpy(rec->last_status, newest->last_status, sizeof rec->last_status);
}

static int EntryMatches(const PinEntry *e, PinEndpointKind kind,
                        const char *label, const char *hostname,
                        uint16_t port)
{
    return e && e->kind == kind && e->port == port &&
           strcmp(e->label, label ? label : "") == 0 &&
           strcmp(e->hostname, hostname ? hostname : "") == 0;
}

static PinEntry *FindEntryLocked(PinEndpointKind kind,
                                 const char *label,
                                 const char *hostname,
                                 uint16_t port)
{
    for (size_t i = 0; i < g_entry_count; i++) {
        if (EntryMatches(&g_entries[i], kind, label, hostname, port)) {
            return &g_entries[i];
        }
    }
    return NULL;
}

static int ReadWholeFile(const wchar_t *path, uint8_t **out, DWORD *out_len)
{
    if (!out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz.QuadPart);
    if (!buf) {
        CloseHandle(h);
        return 0;
    }
    DWORD got = 0;
    int ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart;
    CloseHandle(h);
    if (!ok) {
        free(buf);
        return 0;
    }
    *out = buf;
    *out_len = got;
    return 1;
}

static int ProtectBlob(const uint8_t *plain, DWORD plain_len,
                       DATA_BLOB *out)
{
    if (!plain || plain_len == 0 || !out) return 0;
    DATA_BLOB in;
    in.pbData = (BYTE *)plain;
    in.cbData = plain_len;
    memset(out, 0, sizeof *out);
    // The description is metadata inside the blob; decryption ignores it, so
    // stores written under the old product name keep loading.
    return CryptProtectData(&in, L"Time Actual enrolled pin cache", NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, out) ? 1 : 0;
}

static int UnprotectBlob(const uint8_t *cipher, DWORD cipher_len,
                         DATA_BLOB *out)
{
    if (!cipher || cipher_len == 0 || !out) return 0;
    DATA_BLOB in;
    in.pbData = (BYTE *)cipher;
    in.cbData = cipher_len;
    memset(out, 0, sizeof *out);
    return CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                              CRYPTPROTECT_UI_FORBIDDEN, out) ? 1 : 0;
}

static void ApplyStrictAcl(const wchar_t *path)
{
    HANDLE tok = NULL;
    TOKEN_USER *tu = NULL;
    DWORD need = 0;
    PSID system_sid = NULL;
    PACL acl = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) goto done;
    GetTokenInformation(tok, TokenUser, NULL, 0, &need);
    tu = (TOKEN_USER *)malloc(need);
    if (!tu) goto done;
    if (!GetTokenInformation(tok, TokenUser, tu, need, &need)) goto done;

    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        system_sid = NULL;
    }

    EXPLICIT_ACCESSW ea[2];
    memset(ea, 0, sizeof ea);
    ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = (LPWSTR)tu->User.Sid;

    DWORD count = 1;
    if (system_sid) {
        ea[1].grfAccessPermissions = GENERIC_ALL;
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[1].Trustee.ptstrName = (LPWSTR)system_sid;
        count = 2;
    }
    if (SetEntriesInAclW(count, ea, NULL, &acl) == ERROR_SUCCESS) {
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, acl, NULL);
        if (rc != ERROR_SUCCESS) {
            Log_Append("pinstore: strict ACL apply failed (err=%lu)",
                       (unsigned long)rc);
        }
    }

done:
    if (acl) LocalFree(acl);
    if (system_sid) FreeSid(system_sid);
    free(tu);
    if (tok) CloseHandle(tok);
}

static int WriteProtectedFile(const wchar_t *path, const uint8_t *buf, DWORD len)
{
    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls.tmp", path) < 0) return 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    int ok = WriteFile(h, buf, len, &wrote, NULL) && wrote == len;
    if (ok) ok = FlushFileBuffers(h) ? 1 : 0;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp);
        return 0;
    }
    ApplyStrictAcl(tmp);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    ApplyStrictAcl(path);
    return 1;
}

static int SerializeLocked(char **out_text, DWORD *out_len)
{
    size_t cap = 4096 + g_entry_count * PIN_STORE_MAX_SPKIS * 768;
    char *buf = (char *)malloc(cap);
    if (!buf) return 0;
    size_t pos = 0;
    int n = _snprintf(buf + pos, cap - pos, "LUNAR_PINSTORE|%d\n", PINSTORE_VERSION);
    if (n < 0 || (size_t)n >= cap - pos) { free(buf); return 0; }
    pos += (size_t)n;
    for (size_t i = 0; i < g_entry_count; i++) {
        PinEntry *e = &g_entries[i];
        // One E-line per enrolled SPKI, oldest -> newest, so the load
        // path rebuilds the set in eviction order.
        for (size_t k = 0; k < e->rec.spki_count; k++) {
            const PinSpki *s = &e->rec.spkis[k];
            n = _snprintf(buf + pos, cap - pos,
                "E|%s|%s|%s|%u|%s|%s|%s|%s|%lld|%lld|%lld|%s\n",
                KindName(e->kind), e->label, e->hostname, (unsigned)e->port,
                e->operator_family, s->spki_hex,
                s->not_before, s->not_after,
                (long long)s->not_before_unix,
                (long long)s->not_after_unix,
                (long long)s->renewal_due_unix,
                s->last_status);
            if (n < 0 || (size_t)n >= cap - pos) { free(buf); return 0; }
            pos += (size_t)n;
        }
    }
    *out_text = buf;
    *out_len = (DWORD)pos;
    return 1;
}

static int ParseLine(char *line)
{
    if (strncmp(line, "E|", 2) != 0) return 1;
    char *fields[12];
    size_t n = 0;
    char *p = line + 2;
    while (n < 12) {
        fields[n++] = p;
        char *sep = strchr(p, '|');
        if (!sep) break;
        *sep = 0;
        p = sep + 1;
    }
    if (n != 12) return 0;

    PinEndpointKind kind = ParseKind(fields[0]);
    if (!kind) return 0;
    int port = atoi(fields[3]);
    if (port <= 0 || port > 65535) return 0;

    PinSpki s;
    memset(&s, 0, sizeof s);
    if (!HexToBytes32(fields[5], s.spki)) return 0;
    if (!SafeCopy(s.spki_hex, sizeof s.spki_hex, fields[5])) return 0;
    if (!SafeCopy(s.not_before, sizeof s.not_before, fields[6])) return 0;
    if (!SafeCopy(s.not_after, sizeof s.not_after, fields[7])) return 0;
    s.not_before_unix = _strtoi64(fields[8], NULL, 10);
    s.not_after_unix = _strtoi64(fields[9], NULL, 10);
    s.renewal_due_unix = _strtoi64(fields[10], NULL, 10);
    FormatUnixUtc(s.renewal_due_unix, s.renewal_due, sizeof s.renewal_due);
    if (!SafeCopy(s.last_status, sizeof s.last_status, fields[11])) return 0;

    // Repeated endpoint keys accumulate into one entry's SPKI set in
    // file order (oldest first). A version-1 file never repeats a key
    // and thus loads as single-entry sets.
    PinEntry *e = FindEntryLocked(kind, fields[1], fields[2], (uint16_t)port);
    if (!e) {
        if (g_entry_count >= PINSTORE_MAX_ENTRIES) return 0;
        e = &g_entries[g_entry_count++];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->port = (uint16_t)port;
        if (!SafeCopy(e->label, sizeof e->label, fields[1]) ||
            !SafeCopy(e->hostname, sizeof e->hostname, fields[2]) ||
            !SafeCopy(e->operator_family, sizeof e->operator_family,
                      fields[4])) {
            g_entry_count--;
            return 0;
        }
    }
    RecordUpsertSpkiLocked(&e->rec, &s);
    return 1;
}

static int ParsePlaintextLocked(char *text, DWORD len)
{
    if (!text || len == 0) return 0;
    g_entry_count = 0;
    // Accept the current header and the legacy version-1 header; a v1
    // cache is upgraded in place on the next persist.
    const char header_v2[] = "LUNAR_PINSTORE|2\n";
    const char header_v1[] = "LUNAR_PINSTORE|1\n";
    size_t header_len = sizeof header_v2 - 1;
    if (len < header_len ||
        (strncmp(text, header_v2, header_len) != 0 &&
         strncmp(text, header_v1, header_len) != 0)) {
        return 0;
    }
    char *p = text + header_len;
    while (*p) {
        char *line = p;
        char *nl = strchr(line, '\n');
        if (!nl) return 0;
        *nl = 0;
        if (*line && !ParseLine(line)) return 0;
        p = nl + 1;
    }
    return 1;
}

static int PersistLocked(void)
{
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"pins.dat")) {
        Log_Append("pinstore: cannot build cache path; pins not persisted");
        return 0;
    }

    char *plain = NULL;
    DWORD plain_len = 0;
    if (!SerializeLocked(&plain, &plain_len)) {
        Log_Append("pinstore: serialize failed; pins not persisted");
        return 0;
    }
    DATA_BLOB protected_blob;
    if (!ProtectBlob((const uint8_t *)plain, plain_len, &protected_blob)) {
        Log_Append("pinstore: DPAPI protect failed (err=%lu)",
                   (unsigned long)GetLastError());
        free(plain);
        return 0;
    }
    free(plain);

    Log_Append("pinstore: atomic write start (%u protected bytes)",
               (unsigned)protected_blob.cbData);
    int ok = WriteProtectedFile(path, protected_blob.pbData, protected_blob.cbData);
    if (ok) Log_Append("pinstore: atomic write success (%u endpoints)",
                       (unsigned)g_entry_count);
    else Log_Append("pinstore: atomic write failed (err=%lu)",
                    (unsigned long)GetLastError());
    LocalFree(protected_blob.pbData);
    return ok;
}

int PinStore_Init(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (g_loaded) {
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    g_entry_count = 0;

    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"pins.dat")) {
        Log_Append("pinstore: cache path unavailable; enrollment required");
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }

    uint8_t *cipher = NULL;
    DWORD cipher_len = 0;
    if (!ReadWholeFile(path, &cipher, &cipher_len)) {
        Log_Append("pinstore: cache missing; first-run enrollment required");
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }

    DATA_BLOB plain;
    if (!UnprotectBlob(cipher, cipher_len, &plain)) {
        Log_Append("pinstore: DPAPI decrypt failed (err=%lu); ignoring cache",
                   (unsigned long)GetLastError());
        free(cipher);
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    free(cipher);

    char *text = (char *)malloc(plain.cbData + 1);
    if (!text) {
        LocalFree(plain.pbData);
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    memcpy(text, plain.pbData, plain.cbData);
    text[plain.cbData] = 0;
    int ok = ParsePlaintextLocked(text, plain.cbData);
    free(text);
    LocalFree(plain.pbData);

    if (ok) {
        Log_Append("pinstore: decrypt/parse success (%u endpoints)",
                   (unsigned)g_entry_count);
        for (size_t i = 0; i < g_entry_count; i++) {
            LogPinRecordLocked("loaded", &g_entries[i]);
        }
    } else {
        g_entry_count = 0;
        Log_Append("pinstore: parse/schema failure; ignoring cache and re-enrolling");
    }
    g_loaded = 1;
    LeaveCriticalSection(&g_cs);
    return 1;
}

void PinStore_Shutdown(void)
{
}

int PinStore_GetPin(PinEndpointKind kind,
                    const char *label,
                    const char *hostname,
                    uint16_t port,
                    PinRecord *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!label || !hostname || !port) return 0;
    PinStore_Init();
    EnterCriticalSection(&g_cs);
    PinEntry *e = FindEntryLocked(kind, label, hostname, port);
    if (e && out) *out = e->rec;
    int present = e && e->rec.present;
    LeaveCriticalSection(&g_cs);
    return present ? 1 : 0;
}

int PinStore_ShouldRenew(const PinRecord *rec)
{
    if (!rec || !rec->present) return 1;
    if (rec->renewal_due_unix <= 0) return 1;
    int64_t now = NowUnixSeconds();
    if (now <= 0) return 1;                        // no disciplined time yet
    if (now < rec->renewal_due_unix) return 0;     // not in the renewal window
    // In-window: an upstream that has not rotated yet keeps producing the
    // same past nextCa, so gate the retry on a cooldown -- otherwise every
    // connection forces a fresh CA validation + whole-store write.
    if (rec->last_renew_attempt_unix > 0 &&
        now - rec->last_renew_attempt_unix <
            PINSTORE_RENEWAL_RETRY_COOLDOWN_SECONDS) {
        return 0;
    }
    return 1;
}

int PinStore_IsExpired(const PinRecord *rec)
{
    if (!rec || !rec->present) return 1;
    if (rec->not_after_unix <= 0) return 1;
    int64_t now = NowUnixSeconds();
    return now > 0 && now >= rec->not_after_unix;
}

size_t PinStore_CollectValidSpkis(const PinRecord *rec,
                                  uint8_t out[PIN_STORE_MAX_SPKIS][32])
{
    if (!rec || !rec->present || !out) return 0;
    int64_t now = NowUnixSeconds();
    size_t written = 0;
    for (size_t i = 0; i < rec->spki_count && i < PIN_STORE_MAX_SPKIS; i++) {
        const PinSpki *s = &rec->spkis[i];
        if (s->not_after_unix <= 0) continue;              // unknown expiry
        if (now > 0 && now >= s->not_after_unix) continue; // expired
        memcpy(out[written++], s->spki, 32);
    }
    return written;
}

int PinStore_SavePin(PinEndpointKind kind,
                     const char *label,
                     const char *hostname,
                     uint16_t port,
                     const char *operator_family,
                     const uint8_t spki[32],
                     const char spki_hex[65],
                     const char *not_before,
                     const char *not_after,
                     int64_t not_before_unix,
                     int64_t not_after_unix,
                     const char *status)
{
    if (!label || !hostname || !port || !spki || !spki_hex ||
        !not_before || !not_after || !status) {
        return 0;
    }
    PinStore_Init();
    EnterCriticalSection(&g_cs);
    PinEntry *e = FindEntryLocked(kind, label, hostname, port);
    if (!e) {
        if (g_entry_count >= PINSTORE_MAX_ENTRIES) {
            LeaveCriticalSection(&g_cs);
            Log_Append("pinstore: cannot save %s:%s; cache full",
                       KindName(kind), label);
            return 0;
        }
        e = &g_entries[g_entry_count++];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->port = port;
        SafeCopy(e->label, sizeof e->label, label);
        SafeCopy(e->hostname, sizeof e->hostname, hostname);
        SafeCopy(e->operator_family, sizeof e->operator_family,
                 operator_family ? operator_family : "unknown");
    }

    PinSpki s;
    memset(&s, 0, sizeof s);
    memcpy(s.spki, spki, 32);
    SafeCopy(s.spki_hex, sizeof s.spki_hex, spki_hex);
    SafeCopy(s.not_before, sizeof s.not_before, not_before);
    SafeCopy(s.not_after, sizeof s.not_after, not_after);
    s.not_before_unix = not_before_unix;
    s.not_after_unix = not_after_unix;
    s.renewal_due_unix = ComputeRenewalDue(not_before_unix, not_after_unix);
    FormatUnixUtc(s.renewal_due_unix, s.renewal_due, sizeof s.renewal_due);
    SafeCopy(s.last_status, sizeof s.last_status, status);
    RecordUpsertSpkiLocked(&e->rec, &s);

    // A save only happens after a CA validation, so this is exactly the
    // moment a renewal attempt completed; stamp it so PinStore_ShouldRenew
    // holds off for the cooldown when the upstream cert has not rotated.
    e->rec.last_renew_attempt_unix = NowUnixSeconds();

    LogPinRecordLocked("save", e);
    int ok = PersistLocked();
    LeaveCriticalSection(&g_cs);
    return ok;
}

#ifdef LUNAR_TESTING
void PinStore_TestReset(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    memset(g_entries, 0, sizeof g_entries);
    g_entry_count = 0;
    g_loaded = 0;
    LeaveCriticalSection(&g_cs);
}
#endif