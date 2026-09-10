# Vendored IANA tzdata (compiled zoneinfo)

- Version:     **2026b** (MSYS2 `tzdata` package reports `2026b-dirty`;
  the `-dirty` suffix comes from MSYS2's packaging patches and is
  stripped by `scripts/gen_tz_embed.py`)
- Source:      `C:\msys64\usr\share\zoneinfo` (MSYS2 `tzdata` package)
- Vendored on: 2026-07-04
- Size:        ~830 KB, 319 files
- Licence:     public domain (IANA time zone database)

## Why this exists

`scripts/gen_tz_embed.py` compiles a zoneinfo tree into `src/tz_embed.c`
(the tzdata snapshot embedded in `Lunar.exe`). Reading whatever tree a
build machine happens to have at `C:\msys64\usr\share\zoneinfo` is not
hermetic: two machines with different `tzdata` package versions produce
different binaries from the same commit. This vendored tree is the
generator's default input, so the embedded tzdata is pinned by the repo.

## What is vendored

Only what `gen_tz_embed.py` consumes (not the full ~4.7 MB tree):

- every compiled TZif zone listed in `zone1970.tab` (the canonical
  modern zone list the generator indexes), plus `UTC` / `Etc/UTC`
- the tab files: `zone1970.tab`, `zone.tab`, `iso3166.tab`,
  `zonenow.tab`
- `tzdata.zi` (first line carries the tzdata version string)

## Refresh procedure (release-time, when tzdata updates)

1. Review the timezone-data version pinned in `tools/prereqs.json`. Update
   its archive URL and SHA-256 when adopting a newer upstream package,
   then run `.\kuu.exe run prereqs`.
2. Copy the consumed subset from the checkout's
   `.tools/msys2/ucrt64/share/zoneinfo` into this directory: the zones in
   `zone1970.tab`, `UTC`, `Etc/UTC`, the tab files listed above, and
   `tzdata.zi`. Use Kuu's `fs` operations; no shared installation is involved.
   Review removed and renamed zones as well as new ones.
3. Run `.\kuu.exe run gen-tz`, then `.\kuu.exe run gen-win-tzmap`.
4. Update the version/date above, run `.\kuu.exe run test`, and commit the
   vendored snapshot and generated C files together.

The source path above records the historical import, not a build dependency.
