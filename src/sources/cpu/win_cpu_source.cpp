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

// The Ex form takes a group number as its input buffer and returns that group's rows; the plain
// form only ever reports the calling thread's group. Resolved at runtime because older SDK import
// libraries do not export it.
using FnNtQuerySystemInformationEx = NtStatus(WINAPI*)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);

FnNtQuerySystemInformationEx queryInformationEx()
{
    static FnNtQuerySystemInformationEx fn = []() -> FnNtQuerySystemInformationEx
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<FnNtQuerySystemInformationEx>(
                           GetProcAddress(ntdll, "NtQuerySystemInformationEx"))
                     : nullptr;
    }();
    return fn;
}

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

} // namespace

std::vector<DeviceInfo> WinCpuSource::discover()
{
    std::string name =
        win::regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"ProcessorNameString");
    topo_ = win::queryCpuTopology();

    std::vector<DeviceInfo> devices;
    for (size_t p = 0; p < topo_.packages.size(); ++p)
    {
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::Cpu, int(p)};
        info.name = name.empty() ? "CPU" : name;
        info.attributes["logical_cores"] = std::to_string(topo_.packages[p].processors.size());
        if (topo_.groupSizes.size() > 1)
        {
            info.attributes["processor_groups"] = std::to_string(topo_.groupSizes.size());
        }
        if (topo_.packages.size() > 1)
        {
            info.attributes["packages"] = std::to_string(topo_.packages.size());
        }
        devices.push_back(info);
    }

    ePkg_.resize(topo_.packages.size());
    ePp0_.resize(topo_.packages.size());
    ePp1_.resize(topo_.packages.size());
    baseMhz_ = double(win::regDword(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz"));

    // Ring-0 MSR path (PawnIO). Picks the vendor module and reads the fixed scale factors once.
    std::string vendor =
        win::regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       L"VendorIdentifier");
    if (!pawn_.ok())
    {
        return devices;
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
            for (DeviceInfo& d : devices)
            {
                d.attributes["tjmax_c"] = std::to_string(int(tjMax_));
            }
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
    return devices;
}

void WinCpuSource::readRapl(std::vector<Reading>& out, const DeviceId& dev, uint32_t msr,
                            Energy& st, const std::string& channel, double energyJoule)
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
        out.push_back(Reading{dev, Quantity::Power, Unit::Watt, channel,
                              double(deltaTicks) * energyJoule / dt});
    }
}

double WinCpuSource::sampleMsrClock(const win::PackageInfo& pkg)
{
    if (!msr_ || (vendor_ != Vendor::Intel && vendor_ != Vendor::Amd))
    {
        return 0;
    }
    double maxMhz = 0;
    for (const win::ProcessorRef& pr : pkg.processors)
    {
        // Pin to this processor so the ring-0 read samples its own per-core P-state MSR.
        win::ScopedProcessorPin pin(pr);
        if (!pin.ok())
        {
            continue;
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
    return maxMhz;
}

namespace
{

// Fills `out` with one row per logical processor across every group, in group order. Returns the
// number of rows, which is 0 when the query failed.
int collectPerf(const win::CpuTopology& topo, std::vector<ProcPerf>& out)
{
    out.clear();
    auto queryEx = queryInformationEx();
    if (queryEx && topo.groupSizes.size() > 1)
    {
        for (size_t g = 0; g < topo.groupSizes.size(); ++g)
        {
            std::vector<ProcPerf> rows(size_t(topo.groupSizes[g]));
            USHORT group = USHORT(g);
            ULONG returned = 0;
            if (queryEx(kSystemProcessorPerformanceInformation, &group, sizeof(group), rows.data(),
                        ULONG(rows.size() * sizeof(ProcPerf)), &returned) != 0)
            {
                out.clear();
                return 0;
            }
            size_t got = returned / sizeof(ProcPerf);
            if (got > rows.size())
            {
                got = rows.size();
            }
            out.insert(out.end(), rows.begin(), rows.begin() + ptrdiff_t(got));
        }
        return int(out.size());
    }

    out.resize(size_t(topo.logicalTotal));
    ULONG returned = 0;
    if (NtQuerySystemInformation(kSystemProcessorPerformanceInformation, out.data(),
                                 ULONG(out.size() * sizeof(ProcPerf)), &returned) != 0)
    {
        out.clear();
        return 0;
    }
    out.resize(returned / sizeof(ProcPerf));
    return int(out.size());
}

} // namespace

void WinCpuSource::sample(std::vector<Reading>& out)
{
    int n = topo_.logicalTotal;
    if (n <= 0)
    {
        return;
    }

    std::vector<ProcPerf> perf;
    bool haveLoad = collectPerf(topo_, perf) == n;
    std::vector<Ticks> cur;
    if (haveLoad)
    {
        cur.resize(size_t(n));
        for (int i = 0; i < n; ++i)
        {
            uint64_t idle = uint64_t(perf[i].IdleTime.QuadPart);
            uint64_t total =
                uint64_t(perf[i].KernelTime.QuadPart) + uint64_t(perf[i].UserTime.QuadPart);
            cur[size_t(i)] = {idle, total};
        }
    }
    bool haveDelta = haveLoad && int(prev_.size()) == n;

    // Rated maximum frequency. Group-limited on a multi-group machine - it only fills the calling
    // thread's group - but MaxMhz is a static rated value, identical on every socket.
    ULONG ratedMaxMhz = 0;
    std::vector<ProcPower> power(size_t(n));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                               ULONG(power.size() * sizeof(ProcPower))) == 0)
    {
        for (int i = 0; i < n; ++i)
        {
            ratedMaxMhz = (power[size_t(i)].MaxMhz > ratedMaxMhz) ? power[size_t(i)].MaxMhz
                                                                  : ratedMaxMhz;
        }
    }

    for (size_t p = 0; p < topo_.packages.size(); ++p)
    {
        const win::PackageInfo& pkg = topo_.packages[p];
        DeviceId dev{DeviceKind::Cpu, int(p)};
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
        { out.push_back(Reading{dev, q, u, ch, v}); };

        // --- Load, over this package's own processors ---
        if (haveDelta)
        {
            uint64_t sumTotal = 0, sumIdle = 0;
            for (size_t k = 0; k < pkg.flatIndices.size(); ++k)
            {
                size_t i = size_t(pkg.flatIndices[k]);
                if (i >= cur.size())
                {
                    continue;
                }
                uint64_t dt = cur[i].total - prev_[i].total;
                uint64_t di = cur[i].idle - prev_[i].idle;
                sumTotal += dt;
                sumIdle += di;
                double pct = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
                emit(Quantity::Load, Unit::Percent, "Core " + std::to_string(k), pct);
            }
            if (sumTotal)
            {
                emit(Quantity::Load, Unit::Percent, "Total",
                     100.0 * double(sumTotal - sumIdle) / double(sumTotal));
            }
        }

        // --- Clock ---
        // The per-core P-state MSRs give a true dynamic clock; CurrentMhz from
        // CallNtPowerInformation is unreliable/static on modern CPUs. Without ring-0 access we
        // can't report a live value, so we report base + max clock instead of a misleading
        // static "current".
        double msrClock = sampleMsrClock(pkg);
        if (msrClock > 0)
        {
            emit(Quantity::Clock, Unit::Megahertz, "Core Clock", msrClock);
        }
        else if (baseMhz_ > 0)
        {
            emit(Quantity::Clock, Unit::Megahertz, "Base Clock", baseMhz_);
        }
        if (ratedMaxMhz > 0)
        {
            emit(Quantity::Clock, Unit::Megahertz, "Max Clock", double(ratedMaxMhz));
        }

        // --- Temperature + power (ring-0 MSR via PawnIO) ---
        // Package MSRs are per-socket, so the thread is pinned to a core of this package first.
        if (msr_ && vendor_ == Vendor::Intel)
        {
            win::ScopedProcessorPin pin(pkg.processors.front());
            uint64_t v = 0;
            if (pawn_.readMsr(MSR_IA32_PACKAGE_THERM_STATUS, v))
            {
                double tC = tjMax_ - double((v >> 16) & 0x7F); // readout = degrees below TjMax
                if (tC > 0 && tC < 130)
                {
                    emit(Quantity::Temperature, Unit::Celsius, "Package", tC);
                }
            }
            readRapl(out, dev, MSR_PKG_ENERGY_STATUS, ePkg_[p], "Package Power", energyJoule_);
            readRapl(out, dev, MSR_PP0_ENERGY_STATUS, ePp0_[p], "Cores Power", energyJoule_);
            readRapl(out, dev, MSR_PP1_ENERGY_STATUS, ePp1_[p], "Uncore Power", energyJoule_);
        }
        else if (msr_ && vendor_ == Vendor::Amd)
        {
            win::ScopedProcessorPin pin(pkg.processors.front());
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
            readRapl(out, dev, MSR_AMD_PKG_ENERGY, ePkg_[p], "Package Power", energyJoule_);
        }
        else if (p == 0)
        {
            // No ring-0 MSR path: ARM64, or x86 without PawnIO. ACPI thermal zones are all that
            // is left, and they are not attributable to a socket, so only the first CPU gets one.
            double tC = 0;
            if (acpi_.sample(tC))
            {
                emit(Quantity::Temperature, Unit::Celsius, "Thermal Zone", tC);
            }
        }
    }

    if (haveLoad)
    {
        prev_ = std::move(cur);
    }
}

} // namespace sources
} // namespace hardware_monitor_cpp

#endif // _WIN32
