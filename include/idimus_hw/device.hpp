// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_hw — device identity and descriptive metadata.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace idimus_hw
{

enum class DeviceKind
{
    Cpu,
    GpuIntegrated,
    GpuDiscrete,
    Memory,
    Storage,
    Network,
    Battery,
    Cooler,
    System,
    Other,
};

const char* deviceKindName(DeviceKind k);

// Stable, non-string identity for a device: a (kind, ordinal) pair. Unlike a path string this is
// cheap to compare/hash and carries no formatting assumptions.
struct DeviceId
{
    DeviceKind kind = DeviceKind::Other;
    int ordinal = 0;

    bool operator==(const DeviceId& o) const
    {
        return kind == o.kind && ordinal == o.ordinal;
    }
    bool operator!=(const DeviceId& o) const
    {
        return !(*this == o);
    }
    bool operator<(const DeviceId& o) const
    {
        return kind != o.kind ? kind < o.kind : ordinal < o.ordinal;
    }
};

std::string toString(const DeviceId& id); // e.g. "cpu/0", "gpu-integrated/0"

// Static description of a device, discovered once when the source is opened.
struct DeviceInfo
{
    DeviceId id;
    std::string name;                              // "Apple M1 Pro"
    std::string vendor;                            // optional
    std::map<std::string, std::string> attributes; // free-form: cores, capacity, serial, ...
};

} // namespace idimus_hw
