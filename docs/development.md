# Independent development with Kuu

`kuu.exe` belongs directly in the repository root, beside `tasks.lua`.
Everything else needed for development is fetched or built by Kuu under
`.tools/`. These directories and the executable are ignored by Git. No tool
is discovered in an adjacent checkout or inherited developer-tool directory.

## First setup

1. Copy the published [Kuu 0.5](https://github.com/anafalanx/kuu/releases/tag/0.5)
   or a local Kuu 0.6 build into the project root.
2. Run `.\kuu.exe run prereqs` with network access.
3. Run `.\kuu.exe run test`.

The upstream archives and their SHA-256 digests are listed in
[`tools/prereqs.json`](../tools/prereqs.json). Kuu downloads and checks them,
unpacks a local MSYS2/UCRT64 compiler toolchain and Python, and builds Tcl/Tk
9.0.3 from source in both shared and static forms. The static libraries and
the project's Tcl payload become the single application executable.

Initial provisioning takes several minutes and substantial disk space;
subsequent runs reuse the installed tools. No machine installation, shared
cache, external compiler, Python environment, or Tcl installation is needed.
The downloaded MSYS2 shell is used only to build the upstream Tcl/Tk sources.
Kuu owns the project task graph and supervises all build processes.

Tool directories are private to this checkout. `tools/project.lua` replaces
the child tool search path, clears inherited tool overrides, and puts home,
temporary files, and build caches under `.tools/`. Windows system programs
remain available from the operating system's own directories. Personal app
data is used only by an ordinary `run`; tests and screenshots use scratch data.

## Tasks

Run `.\kuu.exe list` for the complete task list and
`.\kuu.exe run TASK --help` for arguments.

| Task | Purpose |
| --- | --- |
| `prereqs` | Provision compiler, Python and Tcl/Tk; `--all` adds optional tools |
| `env` | Verify and describe the resolved local tools |
| `build` | Build `dist/TimeActual.exe`; `--debug` retains symbols; `--out` changes destination |
| `unit` | Build and run the C engine unit suite |
| `test-tasks` | Exercise task failures, deadlines, JSON and environment isolation |
| `test-prereqs` | Exercise setup, corruption, interrupted downloads, offline repair and relocation |
| `check [exe]` | Test an existing application with isolated data and a deadline |
| `test` | Build, run unit tests, and check the application; the default task |
| `repackage` | Re-append the UI/resources to the existing native build |
| `run` | Run the built app under supervision; `--dev` uses local wish and `lunar.tcl` |
| `icon` | Regenerate icons; accepts `--face`, `--ink`, and `--accent` |
| `build-ext` | Compile the capture extension |
| `shot OUT [--dev]` | Capture the application window without the desktop background |
| `uishot OUT [STATE]` | Capture a deterministic UI state, including settings and event log |
| `test-shot` | Run pixel conversion checks |
| `live-nts` | Explicit live network integration probe, excluded from ordinary tests |
| `probe-nts` | Python diagnostic probe; accepts a list of hosts |
| `gen-tz` | Regenerate C timezone data from the vendored snapshot |
| `gen-win-tzmap` | Regenerate the Windows/IANA mapping with local Go |
| `sign [exe]` | Use the pinned SDK signing tool and active SimplySign session |
| `clean` | Remove `build/` and `dist/`, preserving tools |

Screenshot tasks fetch TWAPI on first use; Windows mapping generation fetches
Go; signing fetches the Windows SDK Build Tools. Each is pinned and stays in
`.tools/`. SimplySign's authenticated certificate provider remains an external
signing prerequisite. Builds and tests do not need signing credentials.

Build/sign operations write a sibling `.sha256` file. Signing is an explicit
task; ordinary builds are local unsigned development artifacts. A running
app may be parked as `TimeActual.exe.old` while a replacement is placed.

`run` stays attached to Kuu. Closing or interrupting Kuu terminates its child
tree. Double-click the built executable for an independent desktop session.

## Validation and concurrency

Use both `.\kuu.exe check` and `.\kuu.exe list` after editing Lua. The first
checks syntax and imports; the second executes declarations and validates
their argument specifications and required fields. Inspect plans with
`.\kuu.exe run --dry-run test`. JSON output uses `.\kuu.exe run --json test`.

Tasks that write shared build state acquire a mutex keyed to the project's
filesystem identity. Archive packages sharing a destination are staged and
inventoried together, preserving overlay order. Each installed directory has
a `.kuu-installed.json` record of relative filenames and SHA-256 digests;
setup verifies file contents before reuse. Missing/corrupt files or invalid
records trigger repair from verified archives. Directory replacement keeps
the previous tree recoverable until the staged installation is promoted.
The old `.tools/installed/` presence stamps are no longer used.

Tcl/Tk invalidates its record before rebuilding and writes a new one only
after all required outputs exist. Its compiled output is rebuilt in place;
an interrupted build can leave partial files and must be rerun. The recipe
is isolated in `tools/tcltk.lua`; formatting changes to the pin list do not
rebuild it. The mbedTLS cache includes its source, configuration, build
recipe, prerequisite pins, and compiler in its key.

To rebuild the toolchain, remove the project's `.tools/` directory while no
task is running and rerun `prereqs`. Keep the root `kuu.exe`. To work offline,
provision the tasks you need first (`prereqs --all` covers every optional tool).

## CI

CI downloads the published Kuu 0.5 executable into the checkout root, verifies
its pinned SHA-256, then runs the same Kuu commands as development. It builds
Tcl/Tk and the entire application, including engine tests and the application
self-test. It caches the checkout's `.tools/` under a key derived from the
prerequisite pins and provisioning code. It neither signs nor publishes.

## Adoption validation (2026-09-10)

The initial toolchain was downloaded and built from the pinned archives.
Validation used the published Kuu 0.5 executable in a separate checkout with
spaces in its path, a clean build directory, invalid inherited tool variables,
and the original compiler/Tcl/Python directories temporarily unavailable.
The build, task regression tests, 2,191 engine checks, application self-test,
screenshot converter and UI capture, timezone generators, live-probe
compilation, icon generation, and repackaging were exercised there.

The nine task regressions cover argument rejection, JSON results, propagation
of nonzero child exits, absent/failing reports, timeout cleanup, and local tool
selection. Application tests clear Tcl library variables and restrict PATH to
Windows system programs, so the executable must use its packaged runtime.
Failures keep their scratch diagnostics under `.tools/tmp/`.
The supervised `run` was checked to stay alive and to terminate its app child
when Kuu exits. The local SDK signing tool's help command also ran successfully.

Tcl path normalization was incorrect inside the coding agent's filesystem
sandbox on this host; the same binaries passed in normal Windows processes.
Use the required execution permission for those GUI/Tcl checks. This is an
execution-environment finding, recorded along with runtime API issues in the
Kuu project's `docs/shortcomings.md`.

Signing requires an active SimplySign session and was not performed as part
of adoption. The source checkpoint push can trigger CI; its remote result
must be checked separately. No release publication is part of the checkpoint.

The local runtime now uses Kuu 0.6's adoption fixes: CLI duration values pass
directly to process timeouts in seconds, aggregate tasks need no empty body,
and archive inventories preserve Unicode names. Small compatibility branches
retain support for the pinned published 0.5 binary used by CI. Installation
inventories now walk extracted files directly; 0.5 needs an explicit
extended-length path when hashing them. The latest 0.6 source fixes that
path boundary natively.

Validation with 0.6 passed the complete build/test task and all 27 cached
prerequisite archive inventories. The task and application checks also passed
with published 0.5, after which the project's root executable was restored to
0.6. Intermittent Windows access-denied failures observed during validation
are retained in Kuu's shortcomings log; their cause has not been isolated.

## Recovery validation checkpoint (2026-09-10)

The new `test-prereqs` fixture passes 17 checks with both 0.5 and 0.6. It uses
tiny archives and a loopback Python server, including corrupt files/records,
interrupted downloads and directory replacement, offline repair, relocation
and concurrency. The full cold toolchain build and recovery/relocation run
were paused before completion. Earlier adoption results above predate the
new installer. See the [dated handoff](handoff-2026-09-10_193511.md) for the
remaining work and local setup state; Step 2 is not complete.

## Cold setup and recovery, second machine (2026-09-10)

Run with the Kuu 0.6 build at its commit 9abab18, from fresh clones with an
empty `.tools`, in normal Windows processes. Logs and the exercise script
stayed local under `.tools/recovery-lab/`.

- **Main checkout.** `prereqs --all` fetched all 27 pinned archives and built
  Tcl/Tk 9.0.3 shared and static in 22 min 50 s. `env` verified gcc 16.2.0,
  Python 3.14.6 and Tcl 9.0.3. The complete `test` task passed in 1 min 41 s:
  9 task checks, 17 recovery checks, the build, 2,191 engine checks and the
  application self-test with `status=ok`.
- **Cold lab, a second clone.** 23 min 18 s to the same state. Then twelve
  fault-injection and relocation steps passed: a changed byte in `ar.exe`
  repaired from the cached archives without a download (445 s: the whole
  MSYS2 destination is re-extracted and re-inventoried); a deleted Python
  record rebuilt (8 s); a corrupt cached Python archive plus a missing
  `python.exe` fetched again, verified and repaired (11 s); a deleted Tcl/Tk
  record rebuilt in place (662 s); the checkout moved to a path with spaces
  with the original path gone and the inherited tool variables replaced by
  junk paths, after which `env`, reuse without downloads or unpacks, offline
  repair from the cache, and the complete `test` task (69 s, every check as
  above) all passed from the moved path.
- **Tcl/Tk cannot be rebuilt from a path with spaces.** The thirteenth step
  removed the Tcl/Tk record in the moved checkout: Tcl 9.0.3's
  `win/configure` and Makefile split the path (`cd: too many arguments`,
  `No rule to make target`). An installed Tcl/Tk keeps working from such a
  path. `tools/tcltk.lua` now refuses the rebuild with that explanation:
  build Tcl/Tk before moving a checkout, or move it to a path without spaces
  first. An upstream build-system limitation, not a Kuu defect.
- **CI.** The checkpoint push succeeded in 7 min 34 s on windows-latest with
  published Kuu 0.5. CI now pins the Kuu 0.6 release.

Two costs worth knowing. Repair works per destination: one damaged byte
re-extracts the whole bundle, minutes for MSYS2. The first verification
after a repair is slower (87 s against 5 to 10 s) because freshly written
files are scanned on first read by the host's on-access scanner.
