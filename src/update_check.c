// update_check.c -- passive GitHub-releases update check. See header.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"

#include "update_check.h"
#include "dns.h"
#include "netutil.h"
#include "pinned_tls.h"
#include "logbuf.h"
#include "version.h"

#define GH_API_HOST   "api.github.com"
// The repository keeps its original name; if it is ever renamed on GitHub
// this path must follow (the API answers a renamed repo with a redirect,
// which this minimal client does not chase).
#define GH_API_PATH   "/repos/anafalanx/lunar/releases/latest"
#define GH_CONNECT_TIMEOUT_MS  6000
#define GH_IO_TIMEOUT_MS       6000
#define GH_MAX_REPLY_BYTES     (64 * 1024)

static volatile LONG g_started = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG    g_cs_ready = 0;
static int   g_available = 0;
static char  g_latest[32] = "";

static void cs_ensure(void) {
    if (InterlockedCompareExchange(&g_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = 2;
    }
    while (g_cs_ready != 2) { Sleep(0); }
}

// Numeric dotted-version compare: <0 / 0 / >0 (missing components = 0).
static int version_cmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    for (int comp = 0; comp < 4; comp++) {
        char *ea, *eb;
        long av = strtol(a, &ea, 10);
        long bv = strtol(b, &eb, 10);
        if (av != bv) return (av < bv) ? -1 : 1;
        a = ea; b = eb;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (*a == 0 && *b == 0) break;
    }
    return 0;
}

#ifdef LUNAR_TESTING
int UpdateCheck_VersionCmp(const char *a, const char *b) {
    return version_cmp(a, b);
}
#endif

// GET the releases/latest JSON over CA-validated TLS and copy the
// "tag_name" value (leading 'v' stripped) into out. Returns 1 on
// success. Resolution is pinned-DoH; the TLS leaf is CA-validated
// against the Windows store (no SPKI pin -- GitHub rotates certs).
static int fetch_latest_tag(char *out, size_t cap) {
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(GH_API_HOST, ip, &fam) != 0) {
        if (Dns_ResolveSystem(GH_API_HOST, ip, &fam) != 0) return 0;
    }

    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, 443, &sa, &salen) == AF_UNSPEC) return 0;
    SOCKET s = Net_ConnectStream(&sa, salen, GH_CONNECT_TIMEOUT_MS,
                                 GH_IO_TIMEOUT_MS);
    if (s == INVALID_SOCKET) return 0;

    int rc = 0;
    uint8_t *reply = NULL;
    PinnedTls tls;
    PinnedTls_Init(&tls);

    static const char *alpn[] = { "http/1.1", NULL };
    PinnedTlsOpenResult openInfo;
    // pin=NULL + allow_ca=1 + force_ca_refresh=1 => pure CA validation.
    if (PinnedTls_OpenEnrolled(&tls, s, GH_API_HOST, alpn, NULL,
                               1 /* allow CA */, 1 /* force CA */,
                               &openInfo) != 0) {
        goto cleanup;
    }
    mbedtls_ssl_context *ssl = PinnedTls_Ssl(&tls);

    char req[256];
    int rlen = _snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: TimeActual\r\n"
        "Accept: application/vnd.github+json\r\n"
        "Connection: close\r\n"
        "\r\n",
        GH_API_PATH, GH_API_HOST);
    if (rlen <= 0 || (size_t)rlen >= sizeof req) goto cleanup;

    for (size_t sent = 0; sent < (size_t)rlen; ) {
        int wr = mbedtls_ssl_write(ssl, (const unsigned char *)req + sent,
                                   (size_t)rlen - sent);
        if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (wr <= 0) goto cleanup;
        sent += (size_t)wr;
    }

    reply = (uint8_t *)malloc(GH_MAX_REPLY_BYTES + 1);
    if (!reply) goto cleanup;
    size_t reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(ssl, reply + reply_len,
                                  GH_MAX_REPLY_BYTES - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            break;   // any read end: parse what we have
        }
        reply_len += (size_t)rd;
        if (reply_len >= GH_MAX_REPLY_BYTES) break;
    }
    reply[reply_len] = 0;

    // Extract "tag_name": "vX.Y.Z" from the raw response (chunk framing
    // never splits a short field in practice -- same approach as els).
    const char *needle = "\"tag_name\"";
    char *p = strstr((char *)reply, needle);
    if (!p) goto cleanup;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') goto cleanup;
    p++;
    if (*p == 'v' || *p == 'V') p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n < cap - 1) n++;
    if (p[n] != '"') goto cleanup;
    memcpy(out, p, n);
    out[n] = 0;
    rc = 1;

    PinnedTls_CloseNotify(&tls);
cleanup:
    PinnedTls_Free(&tls);   // owns the socket
    free(reply);
    return rc;
}

static DWORD WINAPI worker(LPVOID param) {
    (void)param;
    WSADATA wsa;
    int wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    char tag[32] = "";
    if (fetch_latest_tag(tag, sizeof tag)) {
        int newer = version_cmp(tag, LUNAR_VERSION_STR) > 0;
        cs_ensure();
        EnterCriticalSection(&g_cs);
        g_available = newer;
        _snprintf(g_latest, sizeof g_latest, "%s", tag);
        LeaveCriticalSection(&g_cs);
        if (newer) {
            Log_Append("update: v%s available (running %s) -- %s",
                       tag, LUNAR_VERSION_STR, UPDATE_RELEASES_URL);
        } else {
            Log_Append("update: up to date (latest v%s, running %s)",
                       tag, LUNAR_VERSION_STR);
        }
    } else {
        Log_Append("update: check skipped (release info unreachable)");
    }

    if (wsa_ok) WSACleanup();
    return 0;
}

void UpdateCheck_Start(void) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

int UpdateCheck_Available(char *out, size_t cap) {
    cs_ensure();
    EnterCriticalSection(&g_cs);
    int avail = g_available;
    if (avail && out && cap) {
        _snprintf(out, cap, "%s", g_latest);
    }
    LeaveCriticalSection(&g_cs);
    return avail;
}
