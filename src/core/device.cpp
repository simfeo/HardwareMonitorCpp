// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "hardware_monitor_cpp/device.hpp"

namespace hardware_monitor_cpp
{

const char* deviceKindName(DeviceKind k)
{
    switch (k)
    {
        case DeviceKind::Cpu: return "cpu";
        case DeviceKind::GpuIntegrated: return "gpu-integrated";
        case DeviceKind::GpuDiscrete: return "gpu-discrete";
        case DeviceKind::Memory: return "memory";
        case DeviceKind::Storage: return "storage";
        case DeviceKind::Network: return "network";
        case DeviceKind::Battery: return "battery";
        case DeviceKind::Cooler: return "cooler";
        case DeviceKind::System: return "system";
        case DeviceKind::Other: return "other";
    }
    return "other";
}

std::string toString(const DeviceId& id)
{
    return std::string(deviceKindName(id.kind)) + "/" + std::to_string(id.ordinal);
}

} // namespace hardware_monitor_cpp
