#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the console monitor (idimus_monitor).

Usage:  python build_console.py

On Windows the matching signed PawnIO module is installed into a `modules/`
folder next to the executable, so CPU temperature/power work with no extra
steps (run elevated + PawnIO driver installed).
"""
import os
import sys
from build_common import cmake_build, find_output, install_pawnio


def main():
    build_dir = cmake_build("console", "console/build")

    exe = find_output(build_dir, ["idimus_monitor.exe", "idimus_monitor"])
    if not exe:
        print("[x] idimus_monitor was not produced.", file=sys.stderr)
        return 1

    install_pawnio(os.path.dirname(exe))

    print("\n[+] Console monitor build complete.")
    print(f"    executable : {exe}")
    print(f"    run        : {exe} 2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
