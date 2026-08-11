// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/cpu/win_cpu_source.hpp"

#ifdef _WIN32

#include <chrono>
#include <vector>

#include <windows.h>
#include <powerbase.h> // CallNtPowerInformation

#include "platform/windows/win_util.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{
namespace
{

// Intel MSRs (facts; the IntelMSR PawnIO module allows exactly these).
constexpr uint32_t MSR_RAPL_POWER_UNIT = 0x606;
constexpr uint32_t MSR_PKG_ENERGY_STATUS = 0x611;
constexpr uint32_t MSR_PP0_ENERGY_STATUS = 0x639; // cores
constexpr uint32_t MSR_PP1_ENERGY_STATUS = 0x641; // uncore / iGPU
constexpr uint32_t MSR_IA32_TEMPERATURE_TARGET = 0x1A2;
constexpr uint32_t MSR_IA32_PACKAGE_THERM_STATUS = 0x1B1;
constexpr uint32_t MSR_IA32_PERF_STATUS = 0x198; // bits[15:8] = current P-state ratio
constexpr uint32_t MSR_PLATFORM_INFO = 0xCE;     // bits[15:8] = base (non-turbo) ratio

// AMD Zen (family 17h/19h) facts.
constexpr uint32_t MSR_AMD_PWR_UNIT = 0xC0010299;
constexpr uint32_t MSR_AMD_PKG_ENERGY = 0xC001029B;
constexpr uint32_t MSR_AMD_HW_PSTATE_STATUS = 0xC0010293; // CpuFid[7:0], CpuDfsId[13:8]
constexpr uint32_t SMN_THM_CUR_TEMP = 0x00059800; // SMU thermal: bits[31:21] = 0.125C steps

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// System processor performance info (NtQuerySystemInformation class 8). Declared locally - it is
// not in the public SDK headers, but the layout is a stable documented fact.
using NtStatus = LONG;
extern "C" NtStatus WINAPI NtQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct ProcPerf
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; // includes idle
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

// CallNtPowerInformation(ProcessorInformation) row.
struct ProcPower
{
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

int logicalCpuCount()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return int(si.dwNumberOfProcessors);
}

} // namespace

std::vector<DeviceInfo> WinCpuSource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    std::string name =
        win::regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"ProcessorNameString");
    info.name = name.empty() ? "CPU" : name;
    info.attributes["logical_cores"] = std::to_string(logicalCpuCount());
    baseMhz_ = double(win::regDword(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz"));

    // Ring-0 MSR path (PawnIO). Picks the vendor module and reads the fixed scale factors once.
    std::string vendor =
        win::regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"VendorIdentifier");
    if (!pawn_.ok())
    {
        return {info};
    }
    uint64_t v = 0;
    if (vendor == "GenuineIntel" && pawn_.loadModule("IntelMSR"))
    {
        vendor_ = Vendor::Intel;
        if (pawn_.readMsr(MSR_IA32_TEMPERATURE_TARGET, v))
        {
            double tj = double((v >> 16) & 0xFF);
            if (tj > 0 && tj < 130)
            {
                tjMax_ = tj;
            }
        }
        if (pawn_.readMsr(MSR_RAPL_POWER_UNIT, v))
        {
            energyJoule_ = 1.0 / double(1u << unsigned((v >> 8) & 0x1F));
            msr_ = energyJoule_ > 0;
        }
        if (msr_)
        {
            info.attributes["tjmax_c"] = std::to_string(int(tjMax_));
        }
        // Derive MHz-per-ratio from the base frequency and base ratio; fall back to 100 MHz.
        if (msr_ && baseMhz_ > 0 && pawn_.readMsr(MSR_PLATFORM_INFO, v))
        {
            double baseRatio = double((v >> 8) & 0xFF);
            if (baseRatio > 0)
            {
                busClock_ = baseMhz_ / baseRatio;
            }
        }
    }
    else if (vendor == "AuthenticAMD" && pawn_.loadModule("AMDFamily17"))
    {
        vendor_ = Vendor::Amd;
        if (pawn_.readMsr(MSR_AMD_PWR_UNIT, v))
        {
            energyJoule_ = 1.0 / double(1u << unsigned((v >> 8) & 0x1F));
            msr_ = energyJoule_ > 0;
        }
    }
    return {info};
}

void WinCpuSource::readRapl(std::vector<Reading>& out, uint32_t msr, Energy& st,
                            const char* channel, double energyJoule)
{
    uint64_t v = 0;
    if (!pawn_.readMsr(msr, v))
    {
        return;
    }
    uint32_t cur = uint32_t(v & 0xFFFFFFFF);
    double t = nowSeconds();
    if (!st.primed)
    {
        st.last = cur;
        st.t = t;
        st.primed = true;
        return;
    }
    uint32_t deltaTicks = cur - st.last; // unsigned wrap handles the 32-bit counter rollover
    double dt = t - st.t;
    st.last = cur;
    st.t = t;
    if (dt > 0)
    {
        out.push_back(Reading{dev_, Quantity::Power, Unit::Watt, channel,
                              double(deltaTicks) * energyJoule / dt});
    }
}

double WinCpuSource::sampleMsrClock(int n)
{
    if (!msr_ || (vendor_ != Vendor::Intel && vendor_ != Vendor::Amd))
    {
        return 0;
    }
    HANDLE th = GetCurrentThread();
    DWORD_PTR original = 0;
    double maxMhz = 0;
    for (int i = 0; i < n && i < 64; ++i)
    {
        // Pin to core i so the ring-0 read samples that core's per-core P-state MSR.
        DWORD_PTR prev = SetThreadAffinityMask(th, DWORD_PTR(1) << i);
        if (!prev)
        {
            continue;
        }
        if (!original)
        {
            original = prev;
        }
        uint64_t v = 0;
        double f = 0;
        if (vendor_ == Vendor::Intel)
        {
            if (pawn_.readMsr(MSR_IA32_PERF_STATUS, v))
            {
                f = double((v >> 8) & 0xFF) * busClock_;
            }
        }
        else if (pawn_.readMsr(MSR_AMD_HW_PSTATE_STATUS, v))
        {
            double fid = double(v & 0xFF);
            double did = double((v >> 8) & 0x3F);
            if (did > 0)
            {
                f = fid / did * 200.0; // CoreCOF = CpuFid/CpuDfsId * 200 MHz
            }
        }
        if (f > maxMhz)
        {
            maxMhz = f;
        }
    }
    if (original)
    {
        SetThreadAffinityMask(th, original);
    }
    return maxMhz;
}

void WinCpuSource::sample(std::vector<Reading>& out)
{
    int n = logicalCpuCount();
    if (n <= 0)
    {
        return;
    }
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };

    // --- Load ---
    std::vector<ProcPerf> perf(n);
    ULONG returned = 0;
    if (NtQuerySystemInformation(kSystemProcessorPerformanceInformation, perf.data(),
                                 ULONG(perf.size() * sizeof(ProcPerf)), &returned) == 0)
    {
        std::vector<Ticks> cur(n);
        for (int i = 0; i < n; ++i)
        {
            uint64_t idle = uint64_t(perf[i].IdleTime.QuadPart);
            uint64_t total =
                uint64_t(perf[i].KernelTime.QuadPart) + uint64_t(perf[i].UserTime.QuadPart);
            cur[i] = {idle, total};
        }
        if (int(prev_.size()) == n)
        {
            double sum = 0;
            for (int i = 0; i < n; ++i)
            {
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

    // --- Clock ---
    // The per-core P-state MSRs give a true dynamic clock; CurrentMhz from CallNtPowerInformation is
    // unreliable/static on modern CPUs. Without ring-0 access we can't report a live value, so we
    // report base + max clock instead of a misleading static "current".
    double msrClock = sampleMsrClock(n);
    if (msrClock > 0)
    {
        emit(Quantity::Clock, Unit::Megahertz, "Core Clock", msrClock);
    }
    else if (baseMhz_ > 0)
    {
        emit(Quantity::Clock, Unit::Megahertz, "Base Clock", baseMhz_);
    }
    std::vector<ProcPower> power(n);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                               ULONG(power.size() * sizeof(ProcPower))) == 0)
    {
        ULONG maxMhz = 0;
        for (int i = 0; i < n; ++i)
        {
            maxMhz = (power[i].MaxMhz > maxMhz) ? power[i].MaxMhz : maxMhz;
        }
        emit(Quantity::Clock, Unit::Megahertz, "Max Clock", double(maxMhz));
    }

    // --- Temperature + power (ring-0 MSR via PawnIO) ---
    if (msr_ && vendor_ == Vendor::Intel)
    {
        uint64_t v = 0;
        if (pawn_.readMsr(MSR_IA32_PACKAGE_THERM_STATUS, v))
        {
            double tC = tjMax_ - double((v >> 16) & 0x7F); // readout = degrees below TjMax
            if (tC > 0 && tC < 130)
            {
                emit(Quantity::Temperature, Unit::Celsius, "Package", tC);
            }
        }
        readRapl(out, MSR_PKG_ENERGY_STATUS, ePkg_, "Package Power", energyJoule_);
        readRapl(out, MSR_PP0_ENERGY_STATUS, ePp0_, "Cores Power", energyJoule_);
        readRapl(out, MSR_PP1_ENERGY_STATUS, ePp1_, "Uncore Power", energyJoule_);
    }
    else if (msr_ && vendor_ == Vendor::Amd)
    {
        uint64_t v = 0;
        if (pawn_.readSmn(SMN_THM_CUR_TEMP, v))
        {
            double tC = double((v >> 21) & 0x7FF) * 0.125; // Tctl/Tdie
            if (v & (1u << 19))
            {
                tC -= 49.0; // CUR_TEMP_RANGE_SEL
            }
            if (tC > 0 && tC < 130)
            {
                emit(Quantity::Temperature, Unit::Celsius, "Tctl/Tdie", tC);
            }
        }
        readRapl(out, MSR_AMD_PKG_ENERGY, amdPkg_, "Package Power", energyJoule_);
    }
}

} // namespace sources
} // namespace hardware_monitor_cpp

#endif // _WIN32
