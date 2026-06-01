# Notices

## Independent implementation

idimus_hw is an **independent implementation** written from publicly documented facts: operating
system APIs (Win32, IOKit/Mach/sysctl, `/proc` and `/sys`), hardware interface specifications
(JEDEC SPD, NVMe, SMART, MSR/SMN register maps, SMC keys, PCI IDs), and vendor SDK API
descriptions. It is **not derived from, and contains no code copied or translated from, any
MPL-licensed (or other copyleft) hardware-monitoring library.** Only non-copyrightable facts and
interfaces are reused; all expression (architecture, structure, and code) is original.

## Third-party components

idimus_hw does not bundle third-party source into its own code. The following are used at runtime
and retain their own licenses:

| Component | How it is used | License |
| --- | --- | --- |
| **PawnIO modules** (`namazso/PawnIO.Modules`) | Referenced as a git submodule (`third_party/pawnio-modules`, pinned to `0.1.6`). Signed `.bin` modules are loaded at runtime as standalone files; never linked into idimus_hw. | LGPL-2.1-or-later |
| **PawnIOLib** (PawnIO project) | The PawnIO user-mode library, loaded dynamically (`LoadLibrary`) when present, to reach the signed driver. Not bundled. | PawnIO project terms |
| **NVIDIA NVML** (`nvml.dll` / `libnvidia-ml.so`) | Loaded dynamically via the documented public NVML API. Not bundled; ships with the NVIDIA driver. | NVIDIA driver/SDK terms |
| **AMD ADL** (`atiadlxx.dll`) | Loaded dynamically via the documented public ADL API. Not bundled; ships with the AMD driver. | AMD ADL SDK terms |
| **Intel IGCL** (`ControlLib.dll`) | Loaded dynamically via the documented public IGCL API. Not bundled; ships with the Intel driver. | Intel IGCL terms |

Apple frameworks (IOKit, CoreFoundation) and the private IOReport interface on macOS are operating
system components accessed through their normal interfaces; they are not redistributed.

A commercial license for idimus_hw covers only idimus_hw's own code. It does **not** grant rights
to the third-party components above, which remain governed by their respective licenses.
