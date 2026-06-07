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
#elif defined(_WIN32)
#include "sources/battery/win_battery_source.hpp"
#include "sources/cpu/win_cpu_source.hpp"
#include "sources/gpu/amd_gpu_source.hpp"
#include "sources/gpu/intel_gpu_source.hpp"
#include "sources/gpu/nvidia_gpu_source.hpp"
#include "sources/gpu/win_gpu_source.hpp"
#include "sources/memory/win_memory_source.hpp"
#include "sources/network/win_network_source.hpp"
#include "sources/storage/win_storage_source.hpp"
#elif defined(__linux__)
#include "sources/battery/linux_battery_source.hpp"
#include "sources/cpu/linux_cpu_source.hpp"
#include "sources/gpu/linux_drm_gpu_source.hpp"
#include "sources/gpu/nvidia_gpu_source.hpp"
#include "sources/memory/linux_memory_source.hpp"
#include "sources/network/linux_network_source.hpp"
#include "sources/storage/linux_storage_source.hpp"
#endif

namespace idimus_hw
{

std::vector<std::unique_ptr<Source>> createPlatformSources()
{
    std::vector<std::unique_ptr<Source>> sources;
#ifdef __APPLE__
    sources.push_back(std::make_unique<sources::MacCpuSource>());
    sources.push_back(std::make_unique<sources::MacGpuSource>());
    sources.push_back(std::make_unique<sources::MacMemorySource>());
    sources.push_back(std::make_unique<sources::MacNetworkSource>());
    sources.push_back(std::make_unique<sources::MacStorageSource>());
    sources.push_back(std::make_unique<sources::MacBatterySource>());
#elif defined(_WIN32)
    sources.push_back(std::make_unique<sources::WinCpuSource>());
    sources.push_back(std::make_unique<sources::NvidiaGpuSource>()); // NVIDIA discrete (NVML)
    sources.push_back(std::make_unique<sources::AmdGpuSource>());    // AMD discrete (ADL)
    sources.push_back(std::make_unique<sources::IntelGpuSource>());  // Intel Arc discrete (IGCL)
    sources.push_back(std::make_unique<sources::WinGpuSource>());    // Intel integrated (DXGI/PDH)
    sources.push_back(std::make_unique<sources::WinMemorySource>());
    sources.push_back(std::make_unique<sources::WinNetworkSource>());
    sources.push_back(std::make_unique<sources::WinStorageSource>());
    sources.push_back(std::make_unique<sources::WinBatterySource>());
#elif defined(__linux__)
    sources.push_back(std::make_unique<sources::LinuxCpuSource>());
    sources.push_back(std::make_unique<sources::NvidiaGpuSource>());   // NVIDIA (NVML)
    sources.push_back(std::make_unique<sources::LinuxDrmGpuSource>()); // AMD + Intel (sysfs/drm)
    sources.push_back(std::make_unique<sources::LinuxMemorySource>());
    sources.push_back(std::make_unique<sources::LinuxNetworkSource>());
    sources.push_back(std::make_unique<sources::LinuxStorageSource>());
    sources.push_back(std::make_unique<sources::LinuxBatterySource>());
#endif
    return sources;
}

} // namespace idimus_hw
