# hardware_monitor_cpp documentation

The [project README](../README.md) covers what the library is and how to get a first reading out
of it. These pages go deeper.

| Page | What it covers |
| :--- | :--- |
| [Building](building.md) | Build options, per-platform requirements, what each build script produces, embedding in your own CMake project. |
| [Usage](usage.md) | Working with the C++ API: the data model, filtering snapshots, sampling over time, the quantity/unit tables. |
| [Bindings](bindings.md) | Driving the library from Python (or any language with a C FFI) through the C ABI, including the JSON schema. |
| [PawnIO on Windows](pawnio.md) | Installing the driver and the signed module so CPU temperature and package power work, plus troubleshooting. |

## Quick orientation

The library samples hardware into a flat list of readings:

```
Monitor.poll()  ->  Snapshot { devices[], readings[] }
```

A `Reading` is `{ device, quantity, unit, channel, value }`. There is no component tree to walk,
so "every temperature on this machine" is one filter call and the whole snapshot serializes
without a custom visitor. [Usage](usage.md) explains the model in full.

## What needs privileges

Almost nothing. This trips people up, so it is worth stating plainly:

| Works as an ordinary user | Needs administrator / root |
| :--- | :--- |
| CPU load and clock, per-core load | CPU package temperature and RAPL power (ring-0 MSR) |
| GPU load, memory, temperature, power, clock | Some macOS SMC sensors (temperatures, fans) |
| Memory, swap | |
| Storage size, free space, activity, drive temperature | |
| Network throughput | |
| Battery and UPS | |

If the elevated readings are unavailable the library does not fail: those channels are simply
absent from the snapshot, and everything else still reports. On Windows the elevated CPU path
additionally needs PawnIO, which [its own page](pawnio.md) covers.
