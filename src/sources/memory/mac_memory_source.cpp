// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/memory/mac_memory_source.hpp"

#ifdef __APPLE__

#include <cstdint>

#include <mach/mach.h>
#include <sys/sysctl.h>

namespace idimus_hw
{
namespace sources
{

std::vector<DeviceInfo> MacMemorySource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    info.name = "System Memory";
    uint64_t total = 0;
    size_t len = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) == 0)
    {
        info.attributes["total_bytes"] = std::to_string(total);
    }
    return {info};
}

void MacMemorySource::sample(std::vector<Reading>& out)
{
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };

    uint64_t total = 0;
    size_t len = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) != 0)
    {
        return;
    }

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS)
    {
        page = 16384;
    }

    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS)
    {
        return;
    }

    uint64_t avail =
        uint64_t(vm.free_count + vm.inactive_count + vm.speculative_count + vm.purgeable_count) *
        page;
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

    struct xsw_usage swap{};
    size_t sl = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &sl, nullptr, 0) == 0)
    {
        emit(Quantity::DataVolume, Unit::Byte, "Swap Used", double(swap.xsu_used));
        emit(Quantity::DataVolume, Unit::Byte, "Swap Total", double(swap.xsu_total));
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
