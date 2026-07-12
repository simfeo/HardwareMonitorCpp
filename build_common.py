# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Shared helpers for the repo-root build scripts (build_lib/console/server).

Handles CMake configure+build and, on Windows, fully-automatic installation of
the signed PawnIO module that matches this machine's CPU so the built tools can
read CPU temperature / package power with no extra steps.
"""
import os
import sys
import platform
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))

# Must match the third_party/pawnio-modules submodule pin.
PAWNIO_VERSION = "0.1.6"


def is_windows():
    return sys.platform.startswith("win")


def run(cmd, cwd=None):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=cwd)


def _require_cmake():
    if shutil.which("cmake"):
        return
    msg = "[x] 'cmake' was not found on PATH."
    if is_windows():
        msg += (
            "\n    Run this script from the Visual Studio developer prompt so the\n"
            "    compiler and CMake are on PATH:\n"
            "        Start menu -> 'x64 Native Tools Command Prompt for VS 2022'\n"
            "    (or 'ARM64 Native Tools ...' to build for Windows on ARM)."
        )
    else:
        msg += "\n    Install CMake and a C++ compiler, then re-run."
    print(msg, file=sys.stderr, flush=True)
    sys.exit(1)


def cmake_build(src, build_dir, extra_configure=None):
    """Configure (Release) and build a CMake project. Returns the build dir."""
    _require_cmake()
    src = os.path.join(ROOT, src)
    build_dir = os.path.join(ROOT, build_dir)
    args = ["cmake", "-S", src, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"]
    if extra_configure:
        args += extra_configure
    try:
        run(args)
    except subprocess.CalledProcessError:
        # A stale/incompatible cache (e.g. build dir generated from a moved or
        # copied source tree) makes configure abort. Wipe it and reconfigure.
        if os.path.isdir(build_dir):
            print(f"[*] Removing stale build dir and reconfiguring: {build_dir}", flush=True)
            shutil.rmtree(build_dir, ignore_errors=True)
        run(args)
    run(["cmake", "--build", build_dir, "--config", "Release", "--parallel"])
    return build_dir


def find_output(build_dir, names):
    """Recursively find the first produced file matching any of `names`."""
    for root, _dirs, files in os.walk(build_dir):
        for n in names:
            if n in files:
                return os.path.join(root, n)
    return None


# --- PawnIO signed-module auto-install (Windows) ---------------------------

def _cpu_module():
    """Return the PawnIO module base name for this CPU, or None if N/A.

    Intel -> IntelMSR, AMD (Zen / Family 17h+) -> AMDFamily17. ARM64 has no
    x86 MSRs, so there is no module (CPU temp/power stays unavailable).
    """
    arch = platform.machine().lower()
    if "arm" in arch or "aarch64" in arch:
        return None
    vendor = ""
    try:
        import winreg
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
        ) as key:
            vendor, _ = winreg.QueryValueEx(key, "VendorIdentifier")
    except Exception:
        vendor = platform.processor()
    v = (vendor or "").lower()
    if "intel" in v:
        return "IntelMSR"
    if "amd" in v:
        return "AMDFamily17"
    return None


def install_pawnio(dest_dir):
    """Download the signed PawnIO module for this CPU into <dest_dir>/modules.

    No-op on non-Windows. On unsupported CPUs (e.g. ARM64) it warns and skips.
    """
    if not is_windows():
        return
    module = _cpu_module()
    if not module:
        print(
            "[!] No PawnIO module for this CPU/architecture; skipping.\n"
            "    Load and clock still work; CPU temperature/power will read 'n/a'.",
            flush=True,
        )
        return

    ver = PAWNIO_VERSION
    url = (
        "https://github.com/namazso/PawnIO.Modules/releases/download/"
        f"{ver}/release_{ver.replace('.', '_')}.zip"
    )
    print(f"[*] CPU -> {module}.bin; fetching PawnIO modules {ver}", flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        zip_path = os.path.join(tmp, "pawnio.zip")
        req = urllib.request.Request(url, headers={"User-Agent": "idimus-build"})
        with urllib.request.urlopen(req) as resp, open(zip_path, "wb") as f:
            shutil.copyfileobj(resp, f)
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(tmp)

        bin_src = None
        for root, _dirs, files in os.walk(tmp):
            if f"{module}.bin" in files:
                bin_src = os.path.join(root, f"{module}.bin")
                break
        if not bin_src:
            print(f"[x] {module}.bin not found in release {ver}; skipping.", flush=True)
            return

        mod_dir = os.path.join(dest_dir, "modules")
        os.makedirs(mod_dir, exist_ok=True)
        shutil.copy2(bin_src, mod_dir)
        print(f"[+] Installed {module}.bin -> {mod_dir}", flush=True)

    # The signed module needs the PawnIO *driver* too; warn if it's missing.
    pawn_lib = os.path.join(
        os.environ.get("ProgramFiles", r"C:\Program Files"), "PawnIO", "PawnIOLib.dll"
    )
    if not os.path.exists(pawn_lib):
        print(
            "[!] PawnIO driver (PawnIOLib.dll) not found. Install it from "
            "https://pawnio.eu\n    and run the tool as administrator for "
            "CPU temperature/power.",
            flush=True,
        )
