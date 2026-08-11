// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// hardware_monitor_cpp - an immutable point-in-time view: the device table plus every reading.
#pragma once

#include <vector>

#include "hardware_monitor_cpp/device.hpp"
#include "hardware_monitor_cpp/reading.hpp"

namespace hardware_monitor_cpp
{

class Snapshot
{
public:
    Snapshot() = default;
    Snapshot(std::vector<DeviceInfo> devices, std::vector<Reading> readings)
        : devices_(std::move(devices)), readings_(std::move(readings))
    {
    }

    const std::vector<DeviceInfo>& devices() const
    {
        return devices_;
    }
    const std::vector<Reading>& readings() const
    {
        return readings_;
    }

    // Filters (return copies of matching readings; cheap structs).
    std::vector<Reading> forDevice(const DeviceId& id) const;
    std::vector<Reading> forQuantity(Quantity q) const;

    const DeviceInfo* device(const DeviceId& id) const;

private:
    std::vector<DeviceInfo> devices_;
    std::vector<Reading> readings_;
};

} // namespace hardware_monitor_cpp
