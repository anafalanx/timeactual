# Time Actual

A trusted time reference for Windows, presented as a minimalist analog clock.
A hardened C (C23) engine sits beneath a statically-linked Tcl/Tk shell —
one self-contained, signed `.exe`. (Released as *Lunar* up to 0.57; the
first start of 0.58 carries a Lunar installation's pins, settings and logs
over to `%APPDATA%\TimeActual`.)

Time Actual keeps its own cryptographically-attested timescale — disciplined
by authenticated NTS consensus, never by the OS clock — and uses it to
tell you the true time, how certain it is, and **when your PC's own
clock is wrong and by how much**. It is fail-honest: it never silently
lies, and it never goes dark just because the network did.

The version is single-sourced from the top-level [`VERSION`](VERSION) file;
the build injects it into the exe's version resource and `build/version.h`.

## Build and develop

Copy **Kuu 0.6** (`kuu.exe`, 0.5 still works) directly into this repository's root, beside
`tasks.lua`. That is the only tool to supply manually. Use Windows x64 with
network access for the first setup:

```powershell
.\kuu.exe run prereqs       # fetch pinned tools and build Tcl/Tk locally
.\kuu.exe run test          # build the app, run C tests, self-test the exe
.\kuu.exe list              # every task and its arguments
```

Kuu installs nothing on the machine. Every downloaded archive is pinned by
SHA-256 in `tools/prereqs.json`; tools, caches and temporary files live in
`.tools/`. The compiler, Python, and both shared and static Tcl/Tk 9.0.3 are
private to this checkout. Build commands use explicit paths and replace the
inherited tool search path. No workspace manager or adjacent repository is
required. Subsequent builds use the local tools without network access.

```powershell
.\kuu.exe run build         # dist/TimeActual.exe and its SHA-256
.\kuu.exe run unit          # C engine unit tests
.\kuu.exe run check         # isolated application self-test
.\kuu.exe run repackage     # fast Tcl/UI-only rebuild
.\kuu.exe run run           # run the app; Ctrl-C stops it
.\kuu.exe run run --dev     # run lunar.tcl under the local wish
.\kuu.exe run uishot build/ui.png trusted
.\kuu.exe run sign          # local SDK tool; requires an active SimplySign session
.\kuu.exe run clean         # remove build/ and dist/, keep tools
```

The default task is `test`. Add `--json` before a task name for a
machine-readable outcome, or `--dry-run` to inspect its dependency plan.
`kuu.exe check` checks Lua syntax/imports; `kuu.exe list` also validates the
task declarations. See [development notes](docs/development.md) for setup,
optional tools, testing, and CI, and [distribution](docs/distribution.md)
for release signing.

## Run

Double-click `TimeActual.exe`. No other files are needed: Tcl/Tk 9 is linked
in statically, the app and its script libraries ride along as an appended
zipfs image, and the C engine (libgcc + mbedTLS) is archived in.

## Project layout

    src/         # C engine (ntp/nts/dns/clock/pin_store/siv/tz/...) plus
                 #   lunar_main.c (entry point), lunarx.c (::lunar::*),
                 #   lunarclock.c (Direct2D clock widget), cap.c (screenshots)
    lunar.tcl    # the Tk UI chrome: analog face, status bar, Settings, event log
    kuu.exe      # project-local Kuu runtime (ignored by Git)
    tasks.lua    # build, tests, packaging, run, screenshots, signing, codegen
    tools/       # Kuu modules, pinned prerequisites, Tcl packaging/rendering helpers
    assets/      # icons, fonts
    scripts/     # Build/codegen helpers: build.py (mbedTLS archive + version.h),
                 #   gen_tz_embed.py, gen_win_tzmap.go, probe_nts.py
    third_party/ # vendored mbedTLS + IANA tzdata zoneinfo subset
                 #   + CLDR windowsZones (build-time only; see the
                 #   per-directory READMEs under third_party/)
    tests/       # C engine unit tests (run_tests.py) + live NTS probe

## Architecture

- Single static exe: **Tcl/Tk 9 linked in statically**, with the app and
  the Tcl/Tk script libraries appended as a self-mounting zipfs image
  (`TclZipfs_AppHook` + `Tk_Main`). No installer, no runtime deps.
- The **C engine is unchanged** from the native builds — it is exposed to
  the UI as `::lunar::*` commands (`engine_start`, `status`, `sources`,
  `localtime`, `syncnow`, ...) registered from `lunarx.c`. All time logic
  lives in C; Tcl/Tk only draws.
- The UI is an **antialiased Direct2D analog clock** inside normal Tk
  chrome. Its hour markers arm persistent five-minute chimes; the status
  bar reports trust, uncertainty, system-clock delta, and display zone.
  Tk supplies the status-bar settings control and the Settings dialog,
  including its Event Log tab.
- Time is disciplined by six parallel sources: four plain-SNTP
  national-metrology / research-lab servers and two NTS-authenticated
  anchors (RFC 8915, TLS 1.3, local enrolled SPKI pins). The trust gate
  requires two operator-diverse NTS anchors to agree and at least 3 of 4
  core sources to concur.
- The display policy is **fail-honest** and deliberately binary: either
  the time is shown — with its certainty interval drawn as the second
  hand, a fan exactly as wide as the honest error bound — or no time is
  shown at all. The interval starts at the anchor's **measured**
  uncertainty (authenticated pair spread, network asymmetry, and the
  server's own root-dispersion claim — typically a few tens of ms) and
  grows at the worst-case oscillator drift (~12 ms/min) until the next
  authenticated cycle; unauthenticated sources can only ever *widen* it.
  Past a user-settable ceiling (default 5 s) the clock stops showing
  time (**STOPPED**) and recovers as fast as possible. **REACQUIRING**
  covers broken timing continuity (suspend/resume); **ACQUIRING** /
  **NO SIGNAL** cover a run that has not yet, or no longer, anchored.
  There is no intermediate "degraded" tier.
- **System-clock witness.** Time Actual never *displays* the Windows clock,
  but with a disciplined reference in hand it *measures* it: the status
  bar shows "SYS+N.NN", and every step in the OS clock
  (a w32time correction, a manual set, a VM time-sync) is logged with its
  magnitude. This is the one comparison the whole trust stack uniquely
  enables.
- **A real event log.** Its own resizable window (Settings →
  Application → Open event log): a sortable, filterable table over
  everything the engine and UI record — time (in the display zone,
  `~` marking stamps taken before the clock anchored), severity,
  category, message — persisted across sessions in a rolling store
  (`events.log`, rotated at 4 MiB) so yesterday's anomaly is still
  there in the morning.
- **All-day instrument.** An optional run-at-startup entry and a
  confirm-before-closing guard (both in Settings), so it stays up
  rather than being a window you reopen every boot.
- Time zones come from an IANA tzdata snapshot embedded at build time;
  the OS time-zone API is never consulted.

## License

MIT — see [LICENSE](LICENSE). Vendored third-party components keep
their own licenses: mbedTLS (Apache-2.0,
`third_party/mbedtls-3.6.6/LICENSE`), IANA tzdata (public domain,
`third_party/tzdata/README.md`), and CLDR windowsZones (Unicode License
v3, `third_party/cldr/LICENSE`). Overview in `third_party/README.md`.
Tcl/Tk is distributed under the BSD-style Tcl license.
