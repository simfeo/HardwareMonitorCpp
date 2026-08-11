# Usage

Everything lives in namespace `hardware_monitor_cpp` and comes from one umbrella header:

```cpp
#include "hardware_monitor_cpp/hardware_monitor_cpp.hpp"
using namespace hardware_monitor_cpp;
```

## The data model

Five types, and only the first two have behaviour:

```
Monitor       owns Sources, enumerates devices, produces Snapshots
 └─ Source    one data origin (a subsystem on one OS); appends Readings each poll
Snapshot      immutable { devices[], readings[] }; filter by device or quantity
Reading       { device, quantity, unit, channel, value }   <- the atomic unit
DeviceInfo    { id, name, vendor, attributes }             <- what a device *is*
DeviceId      { kind, ordinal }                            <- a stable handle
```

A `Reading` is the whole point. One measured value, tagged with what it measures
(`quantity`), in what unit, on which device, on which channel of that device:

```cpp
struct Reading
{
    DeviceId device;
    Quantity quantity = Quantity::Other;
    Unit unit = Unit::None;
    std::string channel; // "P-Cluster", "Core 3", "en0 rx", "Tdie"
    double value = 0.0;
};
```

There is no tree. A snapshot is a flat vector of these, which is why filtering is a single call
and serializing needs no visitor.

## Lifecycle

```cpp
Monitor m;
m.addPlatformSources();  // every source available on this OS
m.open();                // discover devices once
m.poll();                // prime delta metrics - discard this one
Snapshot s = m.poll();   // a real sample
```

Two things to get right:

**`open()` is called once**, before polling. It enumerates devices.

**The first `poll()` primes rate-based metrics and should be discarded.** CPU load, disk activity
and network throughput are all computed from the difference between two samples, so the first
poll has no predecessor to compare against. Sample it, throw it away, then poll for real. For
rates to be meaningful, leave a sensible interval between polls (the console monitor defaults to
one second).

`Monitor` owns its sources and is not copyable. Keep one alive for the lifetime of your sampling
loop rather than constructing one per sample: reconstructing it re-enumerates devices and throws
away the delta baselines.

## Reading a snapshot

`Snapshot` exposes the raw vectors plus three helpers:

```cpp
const std::vector<DeviceInfo>& devices() const;
const std::vector<Reading>&    readings() const;

std::vector<Reading> forDevice(const DeviceId& id) const;   // all readings of one device
std::vector<Reading> forQuantity(Quantity q) const;         // one quantity across all devices
const DeviceInfo*    device(const DeviceId& id) const;      // id -> descriptive info
```

### Slice by quantity

"Every temperature on this machine", regardless of what produced it:

```cpp
for (const Reading& r : s.forQuantity(Quantity::Temperature))
{
    const DeviceInfo* d = s.device(r.device);
    std::printf("%-18s %-12s %6.1f %s\n", d->name.c_str(), r.channel.c_str(), r.value,
                unitSymbol(r.unit));
}
```

```
Apple M1 Pro       Cores (avg)    50.4 °C
Apple M1 Pro       Cores (max)    61.0 °C
Apple M1 Pro GPU   Die            46.9 °C
Battery            Temperature    30.8 °C
```

### Slice by device

Walk the device table and pull each device's readings:

```cpp
for (const DeviceInfo& d : s.devices())
{
    std::printf("%s [%s]\n", d.name.c_str(), toString(d.id).c_str());
    for (const Reading& r : s.forDevice(d.id))
    {
        std::printf("    %-16s %10.2f %s\n", r.channel.c_str(), r.value, unitSymbol(r.unit));
    }
}
```

That is essentially the whole of
[`examples/hardware_monitor_cpp_dump.cpp`](../examples/hardware_monitor_cpp_dump.cpp), and note
what it does *not* contain: any branch on device type. Adding a new kind of hardware does not
change this code.

### `DeviceId` is a handle, not a name

The most common early mistake. `Reading::device` is a `DeviceId`, which is only
`{ kind, ordinal }` - a cheap, stable, comparable identity. Names, vendors and attributes live
on `DeviceInfo`, which you look up through the snapshot:

```cpp
const DeviceInfo* d = s.device(r.device);   // correct
r.device.name;                              // does not compile: DeviceId has no name
```

`toString(id)` gives a short stable string such as `cpu/0` or `gpu-integrated/0`, useful as a map
key or in logs.

### Picking out one value

There is no single-reading lookup in the API yet, so a small helper is the idiom:

```cpp
const Reading* find(const Snapshot& s, const DeviceId& dev, Quantity q,
                    const std::string& channelContains)
{
    for (const Reading& r : s.readings())
    {
        if (r.device == dev && r.quantity == q &&
            r.channel.find(channelContains) != std::string::npos)
        {
            return &r;
        }
    }
    return nullptr;
}
```

Iterating `readings()` directly like this also avoids the vector copy that `forDevice` and
`forQuantity` return, which matters inside a render loop.

## Sampling over time

```cpp
Monitor m;
m.addPlatformSources();
m.open();
m.poll();                                   // prime

for (;;)
{
    Snapshot s = m.poll();
    // ... use s ...
    std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

Snapshots are independent immutable values, so keeping a history is just keeping the values (or
the few numbers you care about) in your own container. The console monitor stores a ring buffer
per series keyed by `device + quantity + channel`.

## Devices

```cpp
struct DeviceInfo
{
    DeviceId id;
    std::string name;                              // "Apple M1 Pro"
    std::string vendor;                            // optional
    std::map<std::string, std::string> attributes; // free-form
};
```

`attributes` carries whatever a source knows that is not a measurement: core counts, media type,
capacity, BSD name, serial. Keys vary by source and platform, so treat them as optional:

```cpp
auto it = d.attributes.find("media");
bool ssd = it != d.attributes.end() && it->second == "SSD";
```

`DeviceKind` is `Cpu`, `GpuIntegrated`, `GpuDiscrete`, `Memory`, `Storage`, `Network`, `Battery`,
`Cooler`, `System`, `Other`. `deviceKindName(kind)` renders it for display.

## Quantities and units

`Quantity` says what is being measured; `Unit` says in what. `unitSymbol(unit)` returns the
display symbol, so formatting code never has to hard-code one.

| Quantity | Unit | Typical channels |
| :--- | :--- | :--- |
| `Temperature` | `Celsius` | `Cores (avg)`, `Die`, `Tdie`, `Temperature` |
| `Load` | `Percent` | `Total`, `Core 0`, `Usage`, `Activity` |
| `Power` | `Watt` | `Package`, `ANE` |
| `Voltage` / `Current` | `Volt` / `Ampere` | `Voltage`, `Current` |
| `Clock` | `Megahertz` | `P-Cluster`, `E-Cluster`, `Core` |
| `FanSpeed` | `Rpm` | fan name |
| `Energy` | `Joule`, `MilliwattHour` | battery charge as energy |
| `Capacity` | `Byte` | `Free`, `Design Capacity`, `Full Charge Capacity` |
| `DataRate` | `BytePerSecond` | `Download`, `Upload`, `Read`, `Write` |
| `DataVolume` | `Byte` | `Used`, `Total`, `Swap Used`, `Swap Total` |
| `Level` | `Percent` | `Charge`, `Used`, `Health` |
| `Duration` | `Second` | `Runtime`, `Time To Empty` |
| `Count` | `Count` | `Cycle Count` |

Channel names are human labels chosen by each source, so match them loosely
(`channel.find("Total")`) rather than comparing exactly, and always handle absence: a channel
that needs privileges or a vendor runtime simply will not be in the snapshot.

## Absent readings are normal

The library reports what it can reach and stays quiet about the rest. There is no error to
handle when CPU package power is unavailable because you are not elevated, or when NVML is not
installed: those readings are absent. Write display code that tolerates a missing value rather
than assuming a fixed set:

```cpp
const Reading* pw = find(s, cpu, Quantity::Power, "Package");
std::printf("power %s\n", pw ? fmt(pw->value).c_str() : "n/a");
```
