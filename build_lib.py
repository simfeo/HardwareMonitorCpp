#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the idimus_hw core static library (and the idimus_hw_dump example).

Usage:  python build_lib.py

On Windows the matching signed PawnIO module is installed next to the built
example so `idimus_hw_dump` can read CPU temperature/power out of the box.
"""
import os
import sys
from build_common import ROOT, cmake_build, find_output, install_pawnio


def main():
    build_dir = cmake_build(".", "build")

    lib = find_output(build_dir, ["idimus_hw.lib", "libidimus_hw.a"])
    dump = find_output(build_dir, ["idimus_hw_dump.exe", "idimus_hw_dump"])

    if dump:
        # Modules must sit next to the executable that loads them.
        install_pawnio(os.path.dirname(dump))

    print("\n[+] Library build complete.")
    if lib:
        print(f"    static library : {lib}")
    if dump:
        print(f"    example dump   : {dump}")
    print(f"    public headers : {os.path.join(ROOT, 'include', 'idimus_hw')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
