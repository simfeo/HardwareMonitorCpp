// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/igcl.hpp"

#ifdef _WIN32

#include <chrono>
#include <cstring>
#include <vector>

#include <windows.h>

namespace idimus_hw
{
namespace intel
{
namespace
{

using Result = int32_t; // ctl_result_t; 0 == CTL_RESULT_SUCCESS

// ctl_oc_telemetry_item_t: { bool bSupported; ctl_units_t units; ctl_data_type_t type;
// ctl_value_t value; } with natural alignment (matches igcl_api.h compiled by MSVC).
union CtlValue
{
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
};
struct TelemetryItem
{
    bool bSupported;
    int32_t units;
    int32_t
        type; // ctl_data_type_t:
              // 0=int8,1=uint8,2=int16,3=uint16,4=int32,5=uint32,6=int64,7=uint64,8=float,9=double
    CtlValue value;
};

// ctl_data_type_t values used to interpret CtlValue.
double itemValue(const TelemetryItem& it)
{
    switch (it.type)
    {
        case 0: return it.value.i8;
        case 1: return it.value.u8;
        case 2: return it.value.i16;
        case 3: return it.value.u16;
        case 4: return it.value.i32;
        case 5: return it.value.u32;
        case 6: return double(it.value.i64);
        case 7: return double(it.value.u64);
        case 8: return it.value.f32;
        case 9: return it.value.f64;
        default: return it.value.f64;
    }
}

constexpr int CTL_FAN_COUNT = 5;
constexpr int CTL_PSU_COUNT = 5;

// ctl_power_telemetry_t (igcl_api.h order). Size/Version-guarded by IGCL.
struct PowerTelemetry
{
    uint32_t Size;
    uint8_t Version;
    TelemetryItem timeStamp;
    TelemetryItem gpuEnergyCounter;
    TelemetryItem gpuVoltage;
    TelemetryItem gpuCurrentClockFrequency;
    TelemetryItem gpuCurrentTemperature;
    TelemetryItem globalActivityCounter;
    TelemetryItem renderComputeActivityCounter;
    TelemetryItem mediaActivityCounter;
    bool gpuPowerLimited;
    bool gpuTemperatureLimited;
    bool gpuCurrentLimited;
    bool gpuVoltageLimited;
    bool gpuUtilizationLimited;
    TelemetryItem vramEnergyCounter;
    TelemetryItem vramVoltage;
    TelemetryItem vramCurrentClockFrequency;
    TelemetryItem vramCurrentEffectiveFrequency;
    TelemetryItem vramReadBandwidthCounter;
    TelemetryItem vramWriteBandwidthCounter;
    TelemetryItem vramCurrentTemperature;
    bool vramPowerLimited;
    bool vramTemperatureLimited;
    bool vramCurrentLimited;
    bool vramVoltageLimited;
    bool vramBandwidthLimited;
    TelemetryItem fanSpeed[CTL_FAN_COUNT];
    TelemetryItem psu[CTL_PSU_COUNT];
};

// ctl_device_adapter_properties_t — only the leading fields we read (name + type). Over-sized
// buffer keeps us safe if the real struct is larger; we set Size to our own struct size.
struct VersionInfo
{
    uint16_t major;
    uint16_t minor;
};
struct AppId
{
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};
struct InitArgs
{
    uint32_t Size;
    uint8_t Version;
    VersionInfo AppVersion;
    uint32_t flags;
    AppId ApplicationUID;
};

using FnInit = Result(__cdecl*)(InitArgs*, void**);
using FnClose = Result(__cdecl*)(void*);
using FnEnumerate = Result(__cdecl*)(void*, uint32_t*, void**);
using FnTelemetry = Result(__cdecl*)(void*, PowerTelemetry*);

struct Table
{
    FnInit init = nullptr;
    FnClose close = nullptr;
    FnEnumerate enumerate = nullptr;
    FnTelemetry telemetry = nullptr;
};

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Igcl::Igcl()
{
    lib_ = LoadLibraryW(L"ControlLib.dll");
    if (!lib_)
    {
        lib_ = LoadLibraryW(L"igcl.dll");
    }
    if (!lib_)
    {
        return;
    }
    auto m = static_cast<HMODULE>(lib_);
    auto* t = new Table();
    fns_ = t;
    t->init = reinterpret_cast<FnInit>(reinterpret_cast<void*>(GetProcAddress(m, "ctlInit")));
    t->close = reinterpret_cast<FnClose>(reinterpret_cast<void*>(GetProcAddress(m, "ctlClose")));
    t->enumerate = reinterpret_cast<FnEnumerate>(
        reinterpret_cast<void*>(GetProcAddress(m, "ctlEnumerateDevices")));
    t->telemetry = reinterpret_cast<FnTelemetry>(
        reinterpret_cast<void*>(GetProcAddress(m, "ctlPowerTelemetryGet")));
    if (!t->init || !t->enumerate)
    {
        return;
    }

    InitArgs args;
    std::memset(&args, 0, sizeof(args));
    args.Size = sizeof(args);
    args.Version = 0;
    args.AppVersion = {1, 0};
    void* api = nullptr;
    if (t->init(&args, &api) != 0 || !api)
    {
        return;
    }
    api_ = api;

    uint32_t count = 0;
    if (t->enumerate(api, &count, nullptr) != 0 || count == 0)
    {
        ok_ = true;
        return;
    }
    std::vector<void*> handles(count, nullptr);
    if (t->enumerate(api, &count, handles.data()) != 0)
    {
        ok_ = true;
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!handles[i])
        {
            continue;
        }
        AdapterRef a;
        a.handle = handles[i];
        a.name =
            "Intel GPU"; // ctl_device_adapter_properties_t name read is added during validation
        a.discrete = true;
        adapters_.push_back(a);
    }
    prevEnergyJ_.assign(adapters_.size(), -1.0);
    prevTime_.assign(adapters_.size(), 0.0);
    ok_ = true;
}

Igcl::~Igcl()
{
    auto* t = static_cast<Table*>(fns_);
    if (api_ && t && t->close)
    {
        t->close(api_);
    }
    delete t;
    if (lib_)
    {
        FreeLibrary(static_cast<HMODULE>(lib_));
    }
}

GpuReadings Igcl::read(size_t ordinal)
{
    GpuReadings r;
    auto* t = static_cast<Table*>(fns_);
    if (!t || !t->telemetry || ordinal >= adapters_.size())
    {
        return r;
    }

    PowerTelemetry pt;
    std::memset(&pt, 0, sizeof(pt));
    pt.Size = sizeof(pt);
    pt.Version = 0;
    if (t->telemetry(adapters_[ordinal].handle, &pt) != 0)
    {
        return r; // INVALID_SIZE etc. -> fail safe
    }

    if (pt.gpuCurrentTemperature.bSupported)
    {
        r.hasTemp = true;
        r.tempC = itemValue(pt.gpuCurrentTemperature);
    }
    if (pt.gpuCurrentClockFrequency.bSupported)
    {
        r.hasClock = true;
        r.clockMhz = itemValue(pt.gpuCurrentClockFrequency);
    }
    if (pt.globalActivityCounter.bSupported)
    {
        r.hasActivity = true;
        r.activityPct = itemValue(pt.globalActivityCounter);
    }
    if (pt.gpuEnergyCounter.bSupported)
    {
        double energyJ = itemValue(pt.gpuEnergyCounter); // typically joules
        double t2 = nowSeconds();
        if (prevEnergyJ_[ordinal] >= 0)
        {
            double dt = t2 - prevTime_[ordinal];
            double dE = energyJ - prevEnergyJ_[ordinal];
            if (dt > 0 && dE >= 0)
            {
                r.hasPower = true;
                r.powerW = dE / dt;
            }
        }
        prevEnergyJ_[ordinal] = energyJ;
        prevTime_[ordinal] = t2;
    }
    return r;
}

} // namespace intel
} // namespace idimus_hw

#endif // _WIN32
