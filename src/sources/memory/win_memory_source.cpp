// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/memory/win_memory_source.hpp"

#ifdef _WIN32

#include <windows.h>

namespace hardware_monitor_cpp
{
namespace sources
{

std::vector<DeviceInfo> WinMemorySource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    info.name = "System Memory";
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s))
    {
        info.attributes["total_bytes"] = std::to_string(s.ullTotalPhys);
    }
    return {info};
}

void WinMemorySource::sample(std::vector<Reading>& out)
{
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    if (!GlobalMemoryStatusEx(&s))
    {
        return;
    }
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };
    uint64_t usedPhys = s.ullTotalPhys - s.ullAvailPhys;
    emit(Quantity::DataVolume, Unit::Byte, "Used", double(usedPhys));
    emit(Quantity::DataVolume, Unit::Byte, "Available", double(s.ullAvailPhys));
    emit(Quantity::DataVolume, Unit::Byte, "Total", double(s.ullTotalPhys));
    emit(Quantity::Load, Unit::Percent, "Usage", double(s.dwMemoryLoad));

    uint64_t usedPage = s.ullTotalPageFile - s.ullAvailPageFile;
    emit(Quantity::DataVolume, Unit::Byte, "Commit Used", double(usedPage));
    emit(Quantity::DataVolume, Unit::Byte, "Commit Limit", double(s.ullTotalPageFile));

    // Swap == page file: size is commit limit above RAM, used is commit charge above RAM.
    uint64_t swapTotal = s.ullTotalPageFile > s.ullTotalPhys ? s.ullTotalPageFile - s.ullTotalPhys
                                                             : 0;
    uint64_t swapUsed = usedPage > usedPhys ? usedPage - usedPhys : 0;
    if (swapUsed > swapTotal)
    {
        swapUsed = swapTotal;
    }
    emit(Quantity::DataVolume, Unit::Byte, "Swap Used", double(swapUsed));
    emit(Quantity::DataVolume, Unit::Byte, "Swap Total", double(swapTotal));
}

} // namespace sources
} // namespace hardware_monitor_cpp

#endif // _WIN32
