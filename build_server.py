#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the Python web dashboard (C-ABI shared library + server + web assets).

Usage:  python build_server.py

Assembles a ready-to-run folder (the shared library next to idimus_server.py and
web/). On Windows the matching signed PawnIO module is installed alongside so CPU
temperature/power work with no extra steps. Then just:

    cd <dist> && python idimus_server.py      # http://127.0.0.1:8000
"""
import os
import shutil
import sys
from build_common import cmake_build, find_output, install_pawnio


def main():
    build_dir = cmake_build("webserver", "webserver/build")
    dist = os.path.join(build_dir, "dist")

    lib = find_output(
        build_dir,
        ["idimus_hw_c.dll", "libidimus_hw_c.so", "libidimus_hw_c.dylib"],
    )
    if not lib:
        print("[x] The shared library was not produced.", file=sys.stderr)
        return 1

    # Multi-config generators (e.g. Visual Studio) drop the library into a
    # config subfolder; make sure it sits directly next to idimus_server.py.
    os.makedirs(dist, exist_ok=True)
    if os.path.dirname(lib) != dist:
        shutil.copy2(lib, dist)

    install_pawnio(dist)

    print("\n[+] Web dashboard build complete.")
    print(f"    folder : {dist}")
    print(f"    run    : cd \"{dist}\" && python idimus_server.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
