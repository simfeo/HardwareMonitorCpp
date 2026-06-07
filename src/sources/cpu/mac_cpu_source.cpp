// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/cpu/mac_cpu_source.hpp"

#ifdef __APPLE__

#include <algorithm>
#include <string>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>

namespace idimus_hw
{
namespace sources
{
namespace
{

std::string sysctlStr(const char* name)
{
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0)
    {
        return {};
    }
    std::string s(len, '\0');
    if (sysctlbyname(name, &s[0], &len, nullptr, 0) != 0)
    {
        return {};
    }
    if (!s.empty() && s.back() == '\0')
    {
        s.pop_back();
    }
    return s;
}

int sysctlInt(const char* name)
{
    int v = 0;
    size_t len = sizeof(v);
    return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : 0;
}

bool plausibleTemp(float c)
{
    return c > 1.0f && c < 130.0f;
}

void emit(std::vector<Reading>& out, const DeviceId& dev, Quantity q, Unit u,
          const std::string& channel, double value)
{
    out.push_back(Reading{dev, q, u, channel, value});
}

} // namespace

MacCpuSource::MacCpuSource()
{
    // Cache on-die core temperature keys ("Tp..") that read back a plausible value.
    if (smc_.ok())
    {
        for (const std::string& k : smc_.allKeys())
        {
            if (k.size() == 4 && k[0] == 'T' && k[1] == 'p')
            {
                float t;
                if (smc_.readFloat(k, t) && plausibleTemp(t))
                {
                    tempKeys_.push_back(k);
                }
            }
        }
        // Fans: FNum gives the count, F{n}Ac the live RPM.
        float fanCount;
        int n = (smc_.readFloat("FNum", fanCount) && fanCount > 0) ? int(fanCount) : 0;
        for (int i = 0; i < n; ++i)
        {
            std::string key = "F" + std::to_string(i) + "Ac";
            float rpm;
            if (smc_.readFloat(key, rpm))
            {
                fanKeys_.push_back(key);
            }
        }
    }
}

std::vector<DeviceInfo> MacCpuSource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    std::string brand = sysctlStr("machdep.cpu.brand_string");
    info.name = brand.empty() ? "Apple Silicon CPU" : brand;
    info.vendor = "Apple";
    int p = sysctlInt("hw.perflevel0.logicalcpu");
    int e = sysctlInt("hw.perflevel1.logicalcpu");
    if (p > 0)
    {
        info.attributes["performance_cores"] = std::to_string(p);
    }
    if (e > 0)
    {
        info.attributes["efficiency_cores"] = std::to_string(e);
    }
    return {info};
}

void MacCpuSource::sample(std::vector<Reading>& out)
{
    // --- Load (Mach per-core tick deltas) ---
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t infoCount = 0;
    natural_t cpus = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpus, &info, &infoCount) ==
        KERN_SUCCESS)
    {
        auto* load = reinterpret_cast<processor_cpu_load_info_data_t*>(info);
        std::vector<Ticks> cur(cpus);
        for (natural_t i = 0; i < cpus; ++i)
        {
            cur[i].active = load[i].cpu_ticks[CPU_STATE_USER] +
                            load[i].cpu_ticks[CPU_STATE_SYSTEM] + load[i].cpu_ticks[CPU_STATE_NICE];
            cur[i].idle = load[i].cpu_ticks[CPU_STATE_IDLE];
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info),
                      infoCount * sizeof(integer_t));

        if (prevTicks_.size() == cur.size())
        {
            double sum = 0;
            for (size_t i = 0; i < cur.size(); ++i)
            {
                uint64_t da = cur[i].active - prevTicks_[i].active;
                uint64_t di = cur[i].idle - prevTicks_[i].idle;
                uint64_t tot = da + di;
                double pct = tot ? 100.0 * double(da) / double(tot) : 0.0;
                emit(out, dev_, Quantity::Load, Unit::Percent, "Core " + std::to_string(i), pct);
                sum += pct;
            }
            if (!cur.empty())
            {
                emit(out, dev_, Quantity::Load, Unit::Percent, "Total", sum / cur.size());
            }
        }
        prevTicks_ = std::move(cur);
    }

    // --- Temperatures (SMC) ---
    if (!tempKeys_.empty())
    {
        double sum = 0;
        float mx = -1e9f;
        int n = 0;
        for (const std::string& k : tempKeys_)
        {
            float t;
            if (smc_.readFloat(k, t) && plausibleTemp(t))
            {
                sum += t;
                mx = std::max(mx, t);
                ++n;
            }
        }
        if (n > 0)
        {
            emit(out, dev_, Quantity::Temperature, Unit::Celsius, "Cores (avg)", sum / n);
            emit(out, dev_, Quantity::Temperature, Unit::Celsius, "Cores (max)", mx);
        }
    }

    // --- Fans (SMC) ---
    for (size_t i = 0; i < fanKeys_.size(); ++i)
    {
        float rpm;
        if (smc_.readFloat(fanKeys_[i], rpm))
        {
            emit(out, dev_, Quantity::FanSpeed, Unit::Rpm, "Fan " + std::to_string(i + 1), rpm);
        }
    }

    // --- Power + active frequency (IOReport) ---
    if (ioreport_.ok())
    {
        mac::IoReport::Sample s;
        if (ioreport_.sample(s))
        {
            if (s.powerW.count("CPU"))
            {
                emit(out, dev_, Quantity::Power, Unit::Watt, "Package", s.powerW["CPU"]);
            }
            if (s.powerW.count("ANE"))
            {
                emit(out, dev_, Quantity::Power, Unit::Watt, "Neural Engine", s.powerW["ANE"]);
            }
            if (s.freqMhz.count("E-CPU"))
            {
                emit(out, dev_, Quantity::Clock, Unit::Megahertz, "E-Cluster", s.freqMhz["E-CPU"]);
            }
            if (s.freqMhz.count("P-CPU"))
            {
                emit(out, dev_, Quantity::Clock, Unit::Megahertz, "P-Cluster", s.freqMhz["P-CPU"]);
            }
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
