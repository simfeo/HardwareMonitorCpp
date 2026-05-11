# idimus_hw

A cross-platform hardware-telemetry library with a small, data-oriented core. It exposes the
machine's sensors as a flat, queryable stream of **readings** rather than an object tree.

Only non-copyrightable facts are reused (OS API signatures, IOCTL
codes, MSR/SMC keys, JEDEC SPD layouts, NVMe offsets, PCI IDs).

## License

**PolyForm Noncommercial 1.0.0** — free for any non-commercial use. **Commercial use requires a
separate commercial license** (see `LICENSE.md`). Bundled third-party resources keep their own
licenses (see *Third-party resources*).

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
| **Windows** | ✅ CPU — Intel **and** AMD: per-core/total load, clock, name; package/Tctl temperature + RAPL power via ring-0 MSR through PawnIO (Intel verified; AMD implemented). GPU — NVIDIA via NVML (temp, power, util, memory, clocks, fan); AMD + Intel (integrated **and** discrete) via DXGI+PDH (name, load, dedicated/shared memory). Memory; network; storage (disks, size, free, NVMe/ATA temperature); battery/UPS. Verified on i9-9900K + RTX 2080. |
| Linux | planned |

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
