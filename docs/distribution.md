# Distribution guide

How Time Actual gets from a working tree onto a stranger's machine without
SmartScreen scaring them off. Audience: whoever cuts releases.

## 1. Code signing (Authenticode)

Unsigned exes get the red "Windows protected your PC" interstitial and
poor winget/Defender treatment (an unsigned build here has tripped
Defender's `Wacatac` ML heuristic mid-link). Every published Time Actual
binary is Authenticode-signed and RFC-3161 timestamped.

### Certum Open Source Code Signing (what we use)

Time Actual is signed with a **Certum Open Source Code Signing** certificate
through **SimplySign** — the same cloud-key setup used for the author's
other tools (`els`, `drang`). No hardware dongle: the private key lives
in Certum's cloud HSM and is exposed to `signtool` through SimplySign
Desktop.

Publisher on the signature reads
`Open Source Developer Vincent Vercauteren`.

Steps to sign a release:

1. **Connect SimplySign Desktop** and log in (phone OTP). While the
   session is active, the code-signing certificate appears in
   `Cert:\CurrentUser\My` and `signtool /a` will auto-select it.
2. Sign the staged exe:

   ```
   signtool sign /a /tr http://time.certum.pl /td sha256 /fd sha256 /v TimeActual.exe
   ```

   The `/tr … /td sha256` RFC-3161 timestamp is mandatory so the
   signature outlives the certificate.
3. Verify before publishing:

   ```
   signtool verify /pa /all /v TimeActual.exe
   ```

   Confirm the chain terminates at `Certum Trusted Network CA` and the
   output says `The signature is timestamped:`.

> SmartScreen reputation for an OV/open-source cert accrues per-cert and
> per-file; expect a warning on the first downloads of a new cert until
> reputation builds. Keeping the same cert and always timestamping is
> what makes that reputation stick.

### The `sign` build task

`.\kuu.exe run sign` runs the §1 `signtool` command on
`dist/TimeActual.exe` and then verifies it with `signtool verify /pa`, so a
release sign-off is a single task. SimplySign Desktop must be connected
first (so the cert is available to `signtool /a`). Authenticode appends
its certificate table at the end of the file, beside the appended zipfs
image, so re-run `.\kuu.exe run check` on the signed exe afterwards to
confirm it still loads (status must stay `ok`).

## 2. Release checklist

1. **Bump `VERSION`** at the repo root (two-part `X.Y`, e.g. `0.50`; the
   exe's four-part Windows resource fields are this padded with zeros).
2. **Regenerate tzdata if stale** — check IANA for a newer release than
   the embedded one (`src/tz_embed.c`) and regenerate if so.
3. **Build**: `.\kuu.exe run build` (needs the static
   Tcl/Tk 9 payload — see the README Build section). Produces
   `dist/TimeActual.exe`.
4. **Test**: `.\kuu.exe run unit` (C engine unit tests) and
   `.\kuu.exe run check` (headless self-test of the built
   exe). Both must be green — CI must also be green on the release commit.
5. **Sign + verify**: connect SimplySign, then `.\kuu.exe run sign` (or the §1 `signtool` command) on
   `dist/TimeActual.exe`, and verify it. Note the printed SHA-256 of the
   signed exe.
6. **GitHub Release**: tag `vX.Y` (matching VERSION), upload the signed
   exe as the release asset **`TimeActual.exe`** (the build already emits
   that exact casing as `dist/TimeActual.exe`; the download URL is
   case-sensitive, so keep it), and paste its
   SHA-256 into the notes. Time Actual ships as a single self-contained exe —
   the exe *is* the artifact, no installer or archive required.
7. **Round-trip**: download the published asset, re-run
   `signtool verify /pa /all /v` and compare SHA-256 against the notes.
8. **winget PR** (optional): instantiate `packaging/winget/` templates
   with the version, the signed exe's `InstallerUrl`, and its SHA-256,
   validate, and PR to microsoft/winget-pkgs (see
   `packaging/winget/README.md`). The `portable` install type points
   directly at the signed exe.

## 3. Updates and deferred scope

- **Passive update check (shipped).** Time Actual notices when a newer release
  exists by querying the GitHub Releases API over its *own* hardened,
  CA-validated stack (pinned DoH + mbedTLS, no external process) and
  surfaces a notice that links to the release page. It downloads and
  installs nothing — see `src/update_check.c`.
- **Auto-download/swap (deferred).** Fetching, verifying, and swapping
  the binary in place is a security-critical subsystem (signature
  verification of the update, rollback protection) not yet built. Until
  then, users update via winget or by replacing the exe; every release's
  SHA-256 is published in the release notes and the binary is signed, so
  a replacement can be verified before it runs.
- **MSI/MSIX installer (deferred).** The single-file portable exe plus
  winget's `portable` type covers current needs; an installer only earns
  its keep once per-machine installs or auto-update exist.
