# Vendored CLDR windowsZones (Windows time-zone key → IANA mapping)

- File:        `windowsZones.xml`
- Source:      unicode-org/cldr, `common/supplemental/windowsZones.xml`
- Pinned commit: `c33a1f0a23e3b676b406345fe0b42c130defc51d`
  (2025-04-10, "CLDR-18479 Update CLDR data to TZDB 2025b")
- Retrieved:   2026-07-04 from
  `https://raw.githubusercontent.com/unicode-org/cldr/main/common/supplemental/windowsZones.xml`
- `typeVersion` in the file: `2021a` — this is CLDR's own internal tag
  and lags the actual content; the pinned commit above shows the mapping
  was maintained through TZDB 2025b.
- Licence:     Unicode License v3 (CLDR) — full text bundled alongside
  this file in [`LICENSE`](LICENSE); the notice must accompany any
  redistribution of the data.

## Why this exists

The Windows time-zone *key* the OS reports ("Romance Standard Time") is
not an IANA name. IANA tzdata contains no Windows mapping at all — CLDR's
`windowsZones.xml` is the canonical (and only) source for it. Lunar uses
it at build time only, to let a fresh install open on the user's likely
zone instead of UTC. Reading the zone *name* is not trusting the OS
*clock*; time is still disciplined from the network.

## What ships in the binary

**Not this XML.** `scripts/gen_win_tzmap.go` compiles it into
`src/tz_winmap_gen.c` — a sorted C table of `{ Windows key → IANA name }`
pairs (~10 KB in `.rdata`), filtered to the zones actually embedded in
`src/tz_embed.c` and canonicalized through the vendored tzdata backward
links (so CLDR's legacy `Asia/Calcutta` resolves to the embedded
canonical `Asia/Kolkata`). Only that derived table is linked in; this XML
is repo source, kept for reproducible builds.

## Refresh procedure (release-time, when CLDR/tzdata updates)

1. Choose and record an upstream CLDR commit. Use Kuu's `http` module to
   retrieve `common/supplemental/windowsZones.xml` from that exact commit
   under `https://raw.githubusercontent.com/unicode-org/cldr/`. Review the
   diff and record its SHA-256, commit, and retrieval date above.
2. Regenerate the table from the repo root, after any timezone-data bump:

       .\kuu.exe run gen-win-tzmap

3. Run `.\kuu.exe run test` and commit the reviewed XML and generated C
   table together. No global Go, GitHub CLI, or workspace manager is needed.
