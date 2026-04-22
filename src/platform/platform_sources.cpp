// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Assembles the set of telemetry sources for the host platform.
#include "idimus_hw/source.hpp"

#ifdef __APPLE__
#include "sources/battery/mac_battery_source.hpp"
#include "sources/cpu/mac_cpu_source.hpp"
#include "sources/gpu/mac_gpu_source.hpp"
#include "sources/memory/mac_memory_source.hpp"
#include "sources/network/mac_network_source.hpp"
#include "sources/storage/mac_storage_source.hpp"
#endif

namespace idimus_hw {

std::vector<std::unique_ptr<Source>> createPlatformSources() {
    std::vector<std::unique_ptr<Source>> sources;
#ifdef __APPLE__
    sources.push_back(std::make_unique<sources::MacCpuSource>());
    sources.push_back(std::make_unique<sources::MacGpuSource>());
    sources.push_back(std::make_unique<sources::MacMemorySource>());
    sources.push_back(std::make_unique<sources::MacNetworkSource>());
    sources.push_back(std::make_unique<sources::MacStorageSource>());
    sources.push_back(std::make_unique<sources::MacBatterySource>());
#endif
    return sources;
}

} // namespace idimus_hw
