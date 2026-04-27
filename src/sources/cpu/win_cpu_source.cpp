// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/cpu/win_cpu_source.hpp"

#ifdef _WIN32

#include <vector>

#include <windows.h>
#include <powerbase.h> // CallNtPowerInformation

#include "platform/windows/win_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {

// System processor performance info (NtQuerySystemInformation class 8). Declared locally — it is
// not in the public SDK headers, but the layout is a stable documented fact.
using NtStatus = LONG;
extern "C" NtStatus WINAPI NtQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct ProcPerf {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; // includes idle
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

// CallNtPowerInformation(ProcessorInformation) row.
struct ProcPower {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

int logicalCpuCount() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return int(si.dwNumberOfProcessors);
}

} // namespace

std::vector<DeviceInfo> WinCpuSource::discover() {
    DeviceInfo info;
    info.id = dev_;
    std::string name = win::regString(HKEY_LOCAL_MACHINE,
                                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                      L"ProcessorNameString");
    info.name = name.empty() ? "CPU" : name;
    info.attributes["logical_cores"] = std::to_string(logicalCpuCount());
    return {info};
}

void WinCpuSource::sample(std::vector<Reading>& out) {
    int n = logicalCpuCount();
    if (n <= 0)
        return;
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
        out.push_back(Reading{dev_, q, u, ch, v});
    };

    // --- Load ---
    std::vector<ProcPerf> perf(n);
    ULONG returned = 0;
    if (NtQuerySystemInformation(kSystemProcessorPerformanceInformation, perf.data(),
                                 ULONG(perf.size() * sizeof(ProcPerf)), &returned) == 0) {
        std::vector<Ticks> cur(n);
        for (int i = 0; i < n; ++i) {
            uint64_t idle = uint64_t(perf[i].IdleTime.QuadPart);
            uint64_t total = uint64_t(perf[i].KernelTime.QuadPart) + uint64_t(perf[i].UserTime.QuadPart);
            cur[i] = {idle, total};
        }
        if (int(prev_.size()) == n) {
            double sum = 0;
            for (int i = 0; i < n; ++i) {
                uint64_t dt = cur[i].total - prev_[i].total;
                uint64_t di = cur[i].idle - prev_[i].idle;
                double pct = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
                emit(Quantity::Load, Unit::Percent, "Core " + std::to_string(i), pct);
                sum += pct;
            }
            emit(Quantity::Load, Unit::Percent, "Total", sum / n);
        }
        prev_ = std::move(cur);
    }

    // --- Clock (processor power information) ---
    std::vector<ProcPower> power(n);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                               ULONG(power.size() * sizeof(ProcPower))) == 0) {
        unsigned long long sumCur = 0;
        ULONG maxMhz = 0;
        for (int i = 0; i < n; ++i) {
            sumCur += power[i].CurrentMhz;
            maxMhz = (power[i].MaxMhz > maxMhz) ? power[i].MaxMhz : maxMhz;
        }
        emit(Quantity::Clock, Unit::Megahertz, "Core Clock", double(sumCur) / n);
        emit(Quantity::Clock, Unit::Megahertz, "Max Clock", double(maxMhz));
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
