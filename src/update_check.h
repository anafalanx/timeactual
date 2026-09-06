// update_check.h -- passive "a newer release exists" check.
//
// Models els's update notice (els.tcl els::check_update): query the
// GitHub Releases API for the latest tag, compare to the running
// version, and if newer let the UI show a notice that links to the
// release page. It is PASSIVE -- it never downloads or installs
// anything; the published binary is Authenticode-signed and the user
// installs it manually.
//
// The difference from els (which shells out to the OS `curl.exe`, using
// the OS resolver and cert store) is that Lunar runs the check through
// its OWN hardened stack: the hostname is resolved via pinned DoH and
// the TLS session is CA-validated by mbedTLS + the Windows cert store,
// with no external process. CA validation (not SPKI pinning) is used
// because GitHub rotates certs and the check is passive -- a MitM could
// at worst hide or fake the notice, never deliver code.

#ifndef LUNAR_UPDATE_CHECK_H
#define LUNAR_UPDATE_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The release page the notice links to.
#define UPDATE_RELEASES_URL \
    "https://github.com/anafalanx/timeactual/releases/latest"

// Kick the check once, on a detached worker thread (non-blocking).
// Safe to call more than once; only the first call does work.
void UpdateCheck_Start(void);

// If a newer release has been found, copy its version ("0.5.0") into
// `out` and return 1; otherwise return 0.
int  UpdateCheck_Available(char *out, size_t cap);

#ifdef LUNAR_TESTING
// Numeric dotted-version compare, exposed for tests: <0 / 0 / >0.
int  UpdateCheck_VersionCmp(const char *a, const char *b);
#endif

#ifdef __cplusplus
}
#endif

#endif
