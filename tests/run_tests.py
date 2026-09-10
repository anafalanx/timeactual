"""Build and run the Lunar C unit tests.

Usage (from project root):

    .\\kuu.exe run unit

Compiles tests/test_core.c against src/*.c in a separate object set
(without -mwindows so we have a console main) and runs the resulting
binary. Returns non-zero on any check failure.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
TESTS = ROOT / "tests"
BUILD = ROOT / "build" / "tests"

# Reuse build.py's cached mbedTLS archive builder so the tests link
# against exactly the same static library as the shipped binary.
sys.path.insert(0, str(ROOT / "scripts"))
from build import (  # noqa: E402
    build_mbedtls_archive,
    find_tool,
    read_version,
    write_version_header,
    MBEDTLS_DIR,
)


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    gcc = find_tool("gcc")

    # Ensure the mbedTLS static archive is built (cached after first run).
    mbedtls_archive = build_mbedtls_archive(gcc)

    # The engine sources include the generated version header; make sure
    # it exists even when tests run before a full build.
    write_version_header(ROOT / "build", read_version())

    exe = BUILD / "test_core.exe"
    src = [
        TESTS / "test_core.c",   # engine unit tests; links the .c files below
        SRC  / "app_paths.c",
        SRC  / "sysvol.c",
        SRC  / "netutil.c",
        SRC  / "ntp.c",
        SRC  / "clock.c",
        SRC  / "logbuf.c",
        SRC  / "tz.c",
        SRC  / "tzif.c",
        SRC  / "tz_embed.c",
        SRC  / "tz_winmap.c",
        SRC  / "tz_winmap_gen.c",
        SRC  / "siv.c",
        SRC  / "nts_ke.c",
        SRC  / "nts_ef.c",
        SRC  / "pinned_tls.c",
        SRC  / "cert_verify_win.c",
        SRC  / "pin_store.c",
        SRC  / "update_check.c",
        SRC  / "nts.c",
        SRC  / "dns.c",
        SRC  / "h2.c",
    ]
    # No -mwindows: we want a console main(). -Werror to catch new warnings.
    cmd = [
        str(gcc),
        "-O0", "-g",
        "-Wall", "-Wextra", "-Wno-unused-function", "-Werror",
        "-std=c23",
        "-static-libgcc", "-static",
        "-ffunction-sections", "-fdata-sections",
        f"-I{MBEDTLS_DIR / 'include'}",
        f"-I{ROOT / 'third_party'}",
        f"-I{ROOT / 'build'}",
        "-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>",
        "-DLUNAR_TESTING",
        "-o", str(exe),
        *[str(p) for p in src],
        str(mbedtls_archive),
        "-Wl,--gc-sections",
        # Engine link deps only (the Direct2D/DirectWrite shell is retired):
        # winmm (sysvol), ole32 (sysvol COM), ws2_32 (sockets), crypt32 +
        # bcrypt (cert verify / entropy), advapi32, wtsapi32.
        "-lwinmm",
        "-luser32", "-lkernel32", "-lole32", "-lws2_32", "-ladvapi32",
        "-lcrypt32", "-lbcrypt", "-lwtsapi32",
    ]
    print("==> Compiling tests")
    print("   ", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"compile failed (exit {r.returncode})", file=sys.stderr)
        return 2

    print(f"==> Running {exe}")
    r = subprocess.run([str(exe)], cwd=ROOT)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
