# idimus_hw

A cross-platform hardware-telemetry library with a small, data-oriented core. It exposes the
machine's sensors as a flat, queryable stream of **readings** rather than an object tree.

Only non-copyrightable facts are reused (OS API signatures, IOCTL
codes, MSR/SMC keys, JEDEC SPD layouts, NVMe offsets, PCI IDs).

## License

idimus_hw is **dual-licensed**:

- **Free for noncommercial use** under the **PolyForm Noncommercial License 1.0.0**
  ([`LICENSE.md`](LICENSE.md)) — personal, research, education, evaluation, nonprofit.
- **Commercial use requires a paid commercial license** — see [`LICENSING.md`](LICENSING.md).

Contributions are accepted under the [`CLA.md`](CLA.md). Third-party components and the project's
independent-implementation status are documented in [`NOTICE.md`](NOTICE.md).

## Model

```
Monitor              owns Sources, enumerates devices, produces Snapshots
 └─ Source           one data origin (a subsystem on one OS); appends Readings each poll
Snapshot             immutable { devices[], readings[] }; filter by device or quantity
Reading              { device, quantity, unit, channel, value }   ← the atomic unit
DeviceInfo / DeviceId  stable (kind, ordinal) identity + metadata
```

```cpp
#include "idimus_hw/idimus_hw.hpp"
using namespace idimus_hw;

Monitor m;
m.addPlatformSources();
m.open();
m.poll();                          // prime delta metrics
Snapshot s = m.poll();             // sample
for (const Reading& r : s.forQuantity(Quantity::Temperature))
    /* r.device, r.channel, r.value, r.unit */;
```

## Status

| Platform | Sources |
| --- | --- |
| **macOS / Apple Silicon** | ✅ CPU (load, temp, package/ANE power, E/P-cluster frequency, fans), GPU (util, memory, temp, power, frequency), memory (used/avail/swap), network (per-interface bytes + throughput), storage (disks, size, free), battery (charge, capacities, health, voltage, current/power, temp, cycles). Verified on M1 Pro. |
| **Windows** | ✅ CPU — Intel **and** AMD: per-core/total load, clock, name; package/Tctl temperature + RAPL power via ring-0 MSR through PawnIO. GPU — NVIDIA via NVML, Intel integrated via DXGI+PDH (load + memory), AMD via ADL and Intel Arc via IGCL (temp/clocks/activity/power/fan/VRAM). Memory; network; storage (disks, size, free, NVMe/ATA temperature); battery + UPS. **Verified on real hardware:** CPU temp/power on i9-9900K, i7-8550U, **and Ryzen 7 3700X**; NVIDIA GPU (RTX 2080/3060 Ti); **Intel integrated GPU (UHD 620)**; laptop battery + desktop UPS. The **AMD Radeon (ADL)** and **Intel Arc (IGCL)** discrete-GPU telemetry paths are implemented from SDK facts but **not yet verified on Radeon/Arc hardware**. |
| **Linux** | ✅ CPU (per-core/total load via /proc/stat, cpufreq clock, hwmon temperature, RAPL package power), memory (/proc/meminfo), network (/sys/class/net + throughput), storage (/sys/block, size, free, drive temperature), battery (/sys/class/power_supply), GPU — NVIDIA via NVML + AMD/Intel via /sys/class/drm + hwmon (busy %, VRAM, temps, fan, power, clocks). Verified on WSL2 (CPU load, memory, network, NVIDIA); hwmon/RAPL/storage/battery paths are for bare-metal Linux. |

## Building

```sh
git clone --recurse-submodules <repo>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/idimus_hw_dump
```

Some macOS sensors (SMC temperatures, fans) require running as root.

## Third-party resources

Ring-0 access on Windows (CPU temperature + RAPL power) uses the **PawnIO** signed kernel driver
via its official `PawnIOLib.dll`, loaded dynamically. The Pawn module *sources* are referenced as
a git submodule under `third_party/pawnio-modules` (`namazso/PawnIO.Modules`, LGPL-2.1, pinned to
`0.1.6`). The **signed module binary** (`IntelMSR.bin`) is a release artifact and is **not**
bundled — place it in a `modules/` folder next to the executable or set `IDIMUS_PAWNIO_DIR`.
These reads require **administrator** privileges and PawnIO to be installed; without them the CPU
source still reports load and clock.
