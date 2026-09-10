# Vendored third-party source

All third-party code used by Lunar lives in this directory, pinned to a
specific version, with its SHA-256 recorded at the time of vendoring.
The full source is committed to the repo so that:

  1. A fresh clone can build Lunar without network access.
  2. Every file that links into Lunar.exe is auditable in `git log`.
  3. Upgrades are deliberate and reviewable as a single PR.

----------------------------------------------------------------------

## tzdata/

A version-pinned subset of the compiled IANA time zone database
(zoneinfo). It is the hermetic default input to
`scripts/gen_tz_embed.py`, which produces the tzdata snapshot embedded
in `Lunar.exe` (`src/tz_embed.c`). Version, contents, and the refresh
procedure are documented in `tzdata/README.md`.

----------------------------------------------------------------------

## mbedtls-3.6.6/

Mbed TLS 3.6.6 LTS. Used for TLS 1.3 client handshakes during
NTS-KE (Network Time Security, RFC 8915) exchanges with the
authenticated fourth time source.

- Upstream:    https://github.com/Mbed-TLS/mbedtls
- Release tag: mbedtls-3.6.6
- Download:    https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2
- SHA-256:     8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
- Vendored on: 2026-04-21
- LTS support: through at least March 2027
- Licence:     Apache-2.0 OR GPL-2.0-or-later (see LICENSE inside)

### What was removed from the upstream tree

After extracting the tarball we deleted everything Lunar does not
build against, to minimise the audit surface:

  .github/       CI metadata
  3rdparty/      Everest and p256-m alt implementations (we use built-ins)
  ChangeLog.d/   Release-note fragments
  cmake/         CMake modules (we don't use CMake)
  configs/       Sample alternate configs
  docs/          Documentation (read online)
  doxygen/       API doc generator
  framework/     Upstream test harness
  pkgconfig/     .pc files
  programs/      Sample programs
  scripts/       Upstream build/test scripts
  tests/         Upstream test suite and data
  visualc/       MSVC project files
  plus all top-level build-system and dotfiles

What remains: `include/`, `library/`, `LICENSE`, `ChangeLog`,
`README.md`, `SECURITY.md`. That is the TLS/X.509/crypto source and
nothing else.

### Configuration

Our custom config lives in `lunar_mbedtls_config.h` in this directory
(NOT inside the vendored tree, so upstream upgrades don't clobber it).
It is passed to mbedTLS by `-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>`
in `scripts/build.py`. The config disables every mbedTLS feature
Lunar does not need: all server code, all non-TLS-1.3 protocol
versions, PSK, DTLS, file-system I/O, PEM parsing, key generation,
key writing, weak curves, weak hashes, and deprecated APIs.

### Upgrade procedure

1. Check https://mbed-tls.readthedocs.io/en/latest/security-advisories/
   for advisories that touch the modules we enable.
2. Download the new tarball, verify SHA-256 against the release page.
3. `rm -rf third_party/mbedtls-3.6.X/` and re-extract; trim the same
   directories as above.
4. Update the version number in this file, in
   `scripts/build.py` (the `MBEDTLS_DIR` constant), and in
   `lunar_mbedtls_config.h`'s header comment.
5. Build. The cache-key hash of the archive will change, triggering
   a full recompile of the vendored tree. Run the tests
   (`.\kuu.exe run unit`) and a self-test (`.\kuu.exe run check`).
6. Commit as a single commit titled "Bump mbedTLS to X.Y.Z".

----------------------------------------------------------------------

## cldr/

The CLDR `windowsZones.xml` mapping (Windows time-zone key → IANA name).
Used at **build time only** by `scripts/gen_win_tzmap.go` to generate
`src/tz_winmap_gen.c`, so a fresh install can open on the user's likely
zone instead of UTC. The XML itself is **not** linked into `Lunar.exe`.
Pinned commit, provenance, and the refresh procedure are in
`cldr/README.md`.

- Upstream:  unicode-org/cldr, `common/supplemental/windowsZones.xml`
- Licence:   Unicode License v3 — full text in `cldr/LICENSE`; the
  copyright/permission notice must accompany redistribution of the data.
