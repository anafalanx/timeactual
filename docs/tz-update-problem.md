# The tzdata staleness problem

**Status:** Known limitation. No action planned.
**First recorded:** 2026-04-22.

## What Time Actual does today

Time Actual embeds a snapshot of the IANA time-zone database (tzdata) directly
in the executable at build time. The snapshot is produced by
`scripts/gen_tz_embed.py` from whatever `zic`-compiled zoneinfo tree
lives on the build machine (currently MSYS2's
`C:\msys64\usr\share\zoneinfo`). History before Jan 1 of the build year
is discarded; future transitions plus the POSIX TZ footer are kept.

Consequences:
- Time Actual never consults the OS for time-zone information.
- The embedded blob is ~15 KB for 313 canonical zones.
- Correctness is bounded by the tzdata release we bundled at build
  time.

## The problem

The IANA tzdata is a living dataset. Governments change their minds
about time zones (DST abolition, offset realignments, religious
observances). The IANA releases a new tzdata version whenever that
happens -- historically 4-10 times per year, of which typically 1-2
affect a "now" query for *some* zone.

A Time Actual binary built in, say, 2026 and launched in 2030 will use 2026's
rules. For zones whose rules haven't changed (the overwhelming
majority), this is fine forever. For zones that have changed, Time Actual
will show a wall-clock time that is off by up to one hour seasonally,
or occasionally by a fixed amount year-round.

### Who this affects

Estimated impact of a 4-6 year stale binary:

- **~95% of users:** no visible wrongness. Fixed-offset zones (UTC,
  most of Asia, most of Africa) and zones with stable long-running DST
  rules captured by the POSIX footer (US, EU, Australia, NZ) keep
  working indefinitely.
- **~5% of users:** subtle wrongness during certain weeks or months.
  Typical volatile zones: Africa/Casablanca, Asia/Tehran, Asia/Gaza,
  America/Santiago, Pacific/Fiji, America/Asuncion, and whichever
  country most recently changed its DST policy.

### Why users might not notice

- Time Actual is a clock. They read the big digits. The small zone label
  next to it rarely catches the eye.
- If the user is *in* one of the volatile zones, their own OS would
  also be confused unless updated. In that case the discrepancy is a
  sign of the times, not specifically Time Actual's fault.
- If the user is *not* in one of those zones but watches one, they'd
  notice only during the hour-wide DST transition windows.

## Why this hasn't been fixed

The obvious fixes each carry real cost for a cosmetic benefit:

### SQLite + auto-update

- Binary grows by ~700 KB (SQLite amalgamation) to carry 15 KB of data.
- Requires a trusted update channel: an HTTPS endpoint we host, or
  trust in a third party. Adds a signing key to manage, a server to
  keep alive, certificate pinning to maintain, retry/staleness
  semantics to design.
- Doubles Time Actual's network trust surface beyond the existing DoH/NTS
  endpoints authenticated through local enrolled pins.
- Massive complexity win for users who would mostly never see the
  benefit.

### Sidecar file (`lunar.tzdb` next to `TimeActual.exe`)

- Cheap to implement: same blob format, file I/O only, embedded
  fallback if absent.
- Lets power users drop in a fresh blob without redistributing the
  app.
- But: still requires *someone* (us, or a separate updater tool) to
  publish refreshed blobs, and the average user still won't bother.
- Adds a second file to a project whose identity is "single-exe, no
  installer, no config."

### On-demand HTTPS pull from a pinned endpoint

- User clicks "Update time zones" in Settings; Time Actual fetches a signed
  blob from a static URL.
- Requires hosting that endpoint for the life of the application.
- If the endpoint ever goes away (domain lapse, cert expiry, project
  abandoned), older binaries silently lose the ability to refresh --
  arguably *worse* than just shipping a stale bundle, because users
  now have a false sense that their data is fresh.

### Ship a new Time Actual build per tzdata release

- What we effectively do now. The cost is maintainer effort and user
  pressure to "always update."
- Good compromise for active development; bad if the project goes
  dormant for years and the user wants a frozen binary to keep
  working.

## Why we're leaving it alone

Time Actual's architectural value proposition is: **single self-contained
exe, minimal network surface, no installer, no background processes,
no phoning home.** Every plausible fix above contradicts at least one
of those. The staleness problem is real but small, and a user who
cares can always download a newer Time Actual build -- that's already how
operating systems, browsers, and language runtimes handle tzdata
freshness.

For the same reason Go, Python, Chrome, iOS, and every Linux distro
bundle tzdata with the platform and refresh it on platform updates,
Time Actual bundles it with the binary and refreshes it on binary updates.
It's the standard pattern.

## What would make us revisit this

Any of:

1. An actual user reports that Time Actual shows wrong local time in their
   zone because of tzdata staleness.
2. A major zone (one with tens of millions of residents: US,
   EU-member, India, Japan, China, Brazil, Russia, etc.) permanently
   changes its rules in a way that breaks bundled users.
3. Time Actual gains an auto-update mechanism for the binary itself, at
   which point piggy-backing tzdata on that channel becomes trivial.

Until then: we accept the staleness, document it here, and rebuild
whenever a relevant tzdata release lands.

## Operational notes for the maintainer

- To refresh the embedded blob: ensure MSYS2 `tzdata` package is
  current (`pacman -S tzdata`), then run
  `py scripts\gen_tz_embed.py`, rebuild, release.
- `CUTOFF_EPOCH` in `gen_tz_embed.py` is hardcoded to Jan 1 of the
  current year. Bump it at the start of each calendar year to shed
  another year of now-pointless history from the blob.
- IANA announces pending tzdata releases on the `tz-announce`
  mailing list (low volume, ~10 messages/year). Subscribing is the
  cheapest way to know when a refresh is actually worth doing.
