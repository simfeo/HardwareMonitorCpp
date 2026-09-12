# Building

Requirements are deliberately small: a **C++17** compiler and **CMake 3.15+**. The core links
only against the operating system's own libraries, so there is nothing to vendor or install
first. The build scripts additionally need **Python 3**, which every supported platform already
ships.

## The short way

The repo-root scripts configure and build with CMake, then print exactly where the artifacts
landed. On Windows they also download and install the signed PawnIO module matching your CPU, so
CPU temperature and package power work without extra steps.

```sh
python build_lib.py        # core static library + the hardware_monitor_cpp_dump example
python build_console.py    # console/build/bin/hardware_monitor_console
python build_server.py     # webserver/build/dist/ (shared library + server + web assets)
```

They take no arguments. Each one is safe to re-run; CMake reuses the existing build directory.

| Script | Produces | Run it with |
| :--- | :--- | :--- |
| `build_lib.py` | `build/libhardware_monitor_cpp.a` (`.lib` on Windows) and `build/hardware_monitor_cpp_dump` | `./build/hardware_monitor_cpp_dump` |
| `build_console.py` | `console/build/bin/hardware_monitor_console` | `hardware_monitor_console [seconds]` |
| `build_server.py` | `webserver/build/dist/` | `cd <dist> && python hardware_monitor_server.py` |

## Plain CMake

Nothing about the project requires the scripts. To drive the build yourself:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hardware_monitor_cpp_dump
```

The two applications are separate CMake projects, each buildable on its own:

```sh
cmake -S console -B console/build -DCMAKE_BUILD_TYPE=Release && cmake --build console/build
cmake -S webserver -B webserver/build -DCMAKE_BUILD_TYPE=Release && cmake --build webserver/build
```

Building this way on Windows skips the automatic PawnIO module install. Run
[`scripts\windows\setup-pawnio.ps1`](../scripts/windows/setup-pawnio.ps1) yourself, or see
[PawnIO on Windows](pawnio.md).

## Per-platform notes

**macOS.** Links `IOKit` and `CoreFoundation`. Verified on Apple Silicon. Some SMC sensors
(temperatures, fans) only report when running as root; everything else works as a normal user.

**Windows.** Links `advapi32`, `iphlpapi`, `setupapi`, `powrprof`, `ntdll`, `dxgi`, `pdh`,
`ole32`, `oleaut32` and `wbemuuid`, all part of the Windows SDK. NVIDIA telemetry goes through
NVML and Intel/AMD discrete GPUs through IGCL/ADL; each is loaded dynamically at runtime and
simply stays absent when the vendor runtime is not installed. CPU package temperature and power
need PawnIO; without it temperature falls back to ACPI thermal zones over WMI. x64 and ARM64 both
build from the same sources - see [PawnIO on Windows](pawnio.md). Machines with more than 64
logical processors are split into processor groups; load is collected per group and each socket is
reported as its own device, so dual-socket and high-core-count systems report in full.

**Linux.** Links `pthread` and `dl`. Most sources read `/proc` and `/sys` directly. NVML is
loaded with `dlopen` when present. The hwmon, RAPL, storage-temperature and battery paths target
bare-metal Linux; under WSL2 the kernel does not expose them, so those channels are absent while
CPU load, memory, network and NVIDIA GPU still work. x86_64 and aarch64 build from the same
sources; on ARM boards CPU temperature comes from the SoC hwmon or a `/sys/class/thermal` zone,
and package power is absent because RAPL is x86-only. On multi-socket x86 every `coretemp`/
`k10temp` hwmon and every `intel-rapl:N` package domain is read, not just socket 0.

**One device per CPU.** Each physical package is discovered as its own device - `cpu/0`, `cpu/1`,
... - carrying its own load, clock, temperature and power on the usual channel names (`Total`,
`Core N`, `Core Clock`, `Package`). A single-socket machine is therefore unchanged: one `cpu/0`
with exactly the channels it always had. Each device reports its own `logical_cores`, plus
`packages` (and on Windows `processor_groups`, on Linux `numa_nodes`) when there is more than one.

## Submodules

```sh
git clone --recurse-submodules <repo>
```

The only submodule is `third_party/pawnio-modules`, which pins the *sources* of the PawnIO
modules (`namazso/PawnIO.Modules`, LGPL-2.1, tag `0.1.6`). It is reference material and is not
compiled into the library, so an existing clone without submodules still builds. Signed module
binaries are release artifacts and are never bundled; see [PawnIO on Windows](pawnio.md).

## Using it from your own CMake project

The library target is `hardware_monitor_cpp` and it carries its public include directory, so a
consuming target needs one line:

```cmake
add_subdirectory(third_party/HardwareMonitorCpp)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hardware_monitor_cpp)
```

Then include the umbrella header:

```cpp
#include "hardware_monitor_cpp/hardware_monitor_cpp.hpp"
```

If you would rather not use CMake at all, compile the sources under `src/` and add `include/` to
your include path. Link the platform libraries listed above for your target.

## Verifying a build

`hardware_monitor_cpp_dump` prints one snapshot: every device, its attributes and every reading.
It is the fastest way to confirm the build sees your hardware, and it doubles as a worked example
of the API (about 20 lines, in
[`examples/hardware_monitor_cpp_dump.cpp`](../examples/hardware_monitor_cpp_dump.cpp)).

```
hardware_monitor_cpp - 22 device(s)

== Apple M1 Pro  [cpu/0] ==
   (efficiency_cores: 2)
   (performance_cores: 8)
   Core 0                37.62 %
   ...
   Cores (avg)           50.41 °C
```

If a device you expect is missing, check the privilege table in the
[documentation index](README.md) before assuming the build is at fault.
