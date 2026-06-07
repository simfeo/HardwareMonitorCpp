// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/memory/linux_memory_source.hpp"

#ifdef __linux__

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>

namespace idimus_hw
{
namespace sources
{
namespace
{
std::map<std::string, uint64_t> readMeminfo()
{
    std::map<std::string, uint64_t> m;
    std::ifstream f("/proc/meminfo");
    std::string key;
    uint64_t val;
    std::string unit;
    while (f >> key >> val)
    {
        std::getline(f, unit);
        if (!key.empty() && key.back() == ':')
        {
            key.pop_back();
        }
        m[key] = val * 1024; // values are in kB
    }
    return m;
}
} // namespace

std::vector<DeviceInfo> LinuxMemorySource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    info.name = "System Memory";
    auto m = readMeminfo();
    if (m.count("MemTotal"))
    {
        info.attributes["total_bytes"] = std::to_string(m["MemTotal"]);
    }
    return {info};
}

void LinuxMemorySource::sample(std::vector<Reading>& out)
{
    auto m = readMeminfo();
    if (!m.count("MemTotal"))
    {
        return;
    }
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };
    uint64_t total = m["MemTotal"];
    uint64_t avail = m.count("MemAvailable") ? m["MemAvailable"] : m["MemFree"];
    if (avail > total)
    {
        avail = total;
    }
    uint64_t used = total - avail;
    emit(Quantity::DataVolume, Unit::Byte, "Used", double(used));
    emit(Quantity::DataVolume, Unit::Byte, "Available", double(avail));
    emit(Quantity::DataVolume, Unit::Byte, "Total", double(total));
    emit(Quantity::Load, Unit::Percent, "Usage",
         total ? 100.0 * double(used) / double(total) : 0.0);

    if (m.count("SwapTotal"))
    {
        uint64_t st = m["SwapTotal"], sf = m.count("SwapFree") ? m["SwapFree"] : 0;
        emit(Quantity::DataVolume, Unit::Byte, "Swap Used", double(st - sf));
        emit(Quantity::DataVolume, Unit::Byte, "Swap Total", double(st));
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
