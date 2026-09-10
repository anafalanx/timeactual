"""Build + run tests/live_nts.exe -- hits real providers, not part of CI."""
import argparse, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from build import build_mbedtls_archive, MBEDTLS_DIR, find_tool, write_version_header
GCC = find_tool("gcc")

ROOT = Path(__file__).resolve().parent.parent
SRC, BUILD = ROOT/"src", ROOT/"build"
(BUILD/"tests").mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-only", action="store_true")
args = parser.parse_args()
write_version_header()
arc = build_mbedtls_archive(GCC)
exe = BUILD/"tests"/"live_nts.exe"
engine = "app_paths sysvol netutil ntp clock logbuf tz tzif tz_embed tz_winmap tz_winmap_gen siv nts_ke nts_ef pinned_tls cert_verify_win pin_store update_check nts dns h2".split()
cmd = [GCC, "-O2", "-Wall", "-std=c23", "-static", "-static-libgcc",
       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
       f"-I{MBEDTLS_DIR/'include'}", f"-I{ROOT/'third_party'}",
       f"-I{BUILD}",
       "-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>",
       "-o", str(exe),
       str(ROOT/"tests"/"live_nts.c"),
       *[str(SRC/(name + ".c")) for name in engine],
       str(arc), "-lws2_32", "-ladvapi32", "-lbcrypt", "-lcrypt32",
       "-lwinmm", "-luser32", "-lkernel32", "-lole32", "-lwtsapi32"]
r = subprocess.run(cmd)
if r.returncode != 0: sys.exit(r.returncode)
if args.build_only:
    print(f"Built {exe}")
    sys.exit(0)
sys.exit(subprocess.run([str(exe)]).returncode)
