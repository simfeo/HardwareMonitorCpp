// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/adl.hpp"

#ifdef _WIN32

#include <cstdlib>
#include <cstring>
#include <set>

#include <windows.h>

namespace idimus_hw
{
namespace amd
{
namespace
{

constexpr int ADL_OK = 0;
constexpr int ADL_VENDOR_AMD = 1002;
constexpr int ADL_PMLOG_MAX_SENSORS = 256;

// ADL AdapterInfo (Windows layout) — full struct so the array stride is correct.
struct AdapterInfo
{
    int iSize;
    int iAdapterIndex;
    char strUDID[256];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[256];
    char strDisplayName[256];
    int iPresent;
    int iExist;
    char strDriverPath[256];
    char strDriverPathExt[256];
    char strPNPString[256];
    int iOSDisplayIndex;
};

struct SingleSensor
{
    int supported;
    int value;
};
struct PmLogDataOutput
{
    int size;
    SingleSensor sensors[ADL_PMLOG_MAX_SENSORS];
};

using MallocCb = void*(__stdcall*)(int);
void* __stdcall adlMalloc(int n)
{
    return std::malloc(size_t(n));
}

using FnCreate = int(__stdcall*)(MallocCb, int, void**);
using FnDestroy = int(__stdcall*)(void*);
using FnNumAdapters = int(__stdcall*)(void*, int*);
using FnAdapterInfo = int(__stdcall*)(void*, AdapterInfo*, int);
using FnActive = int(__stdcall*)(void*, int, int*);
using FnQueryPmLog = int(__stdcall*)(void*, int, PmLogDataOutput*);
using FnVram = int(__stdcall*)(void*, int, int*);

struct Table
{
    FnDestroy destroy = nullptr;
    FnNumAdapters numAdapters = nullptr;
    FnAdapterInfo adapterInfo = nullptr;
    FnActive active = nullptr;
    FnQueryPmLog queryPmLog = nullptr;
    FnVram vram = nullptr;
};

template <class T> T sym(HMODULE m, const char* n)
{
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, n)));
}

} // namespace

Adl::Adl()
{
    lib_ = LoadLibraryW(L"atiadlxx.dll");
    if (!lib_)
    {
        lib_ = LoadLibraryW(L"atiadlxy.dll");
    }
    if (!lib_)
    {
        return;
    }
    auto m = static_cast<HMODULE>(lib_);

    auto create = sym<FnCreate>(m, "ADL2_Main_Control_Create");
    auto* t = new Table();
    fns_ = t;
    t->destroy = sym<FnDestroy>(m, "ADL2_Main_Control_Destroy");
    t->numAdapters = sym<FnNumAdapters>(m, "ADL2_Adapter_NumberOfAdapters_Get");
    t->adapterInfo = sym<FnAdapterInfo>(m, "ADL2_Adapter_AdapterInfo_Get");
    t->active = sym<FnActive>(m, "ADL2_Adapter_Active_Get");
    t->queryPmLog = sym<FnQueryPmLog>(m, "ADL2_New_QueryPMLogData_Get");
    t->vram = sym<FnVram>(m, "ADL2_Adapter_DedicatedVRAMUsage_Get");

    if (!create || !t->numAdapters || !t->adapterInfo)
    {
        return;
    }
    void* ctx = nullptr;
    if (create(adlMalloc, 1, &ctx) != ADL_OK || !ctx)
    {
        return;
    }
    ctx_ = ctx;

    int num = 0;
    if (t->numAdapters(ctx, &num) != ADL_OK || num <= 0)
    {
        ok_ = true; // library is up, just no adapters
        return;
    }
    std::vector<AdapterInfo> info(static_cast<size_t>(num));
    std::memset(info.data(), 0, info.size() * sizeof(AdapterInfo));
    if (t->adapterInfo(ctx, info.data(), int(info.size() * sizeof(AdapterInfo))) == ADL_OK)
    {
        std::set<int> seen; // dedupe physical GPUs by bus/device/function
        for (const AdapterInfo& ai : info)
        {
            if (ai.iVendorID != ADL_VENDOR_AMD || !ai.iPresent)
            {
                continue;
            }
            int active = 0;
            if (t->active && t->active(ctx, ai.iAdapterIndex, &active) == ADL_OK && !active)
            {
                continue;
            }
            int key = (ai.iBusNumber << 16) | (ai.iDeviceNumber << 8) | ai.iFunctionNumber;
            if (!seen.insert(key).second)
            {
                continue;
            }
            adapters_.push_back(AdapterRef{ai.iAdapterIndex, ai.strAdapterName});
        }
    }
    ok_ = true;
}

Adl::~Adl()
{
    auto* t = static_cast<Table*>(fns_);
    if (ctx_ && t && t->destroy)
    {
        t->destroy(ctx_);
    }
    delete t;
    if (lib_)
    {
        FreeLibrary(static_cast<HMODULE>(lib_));
    }
}

std::map<int, double> Adl::queryPmLog(int adapterIndex)
{
    std::map<int, double> result;
    auto* t = static_cast<Table*>(fns_);
    if (!t || !t->queryPmLog || !ctx_)
    {
        return result;
    }
    PmLogDataOutput out;
    std::memset(&out, 0, sizeof(out));
    if (t->queryPmLog(ctx_, adapterIndex, &out) != ADL_OK)
    {
        return result;
    }
    for (int i = 0; i < ADL_PMLOG_MAX_SENSORS; ++i)
    {
        if (out.sensors[i].supported)
        {
            result[i] = double(out.sensors[i].value);
        }
    }
    return result;
}

int Adl::dedicatedVramMb(int adapterIndex)
{
    auto* t = static_cast<Table*>(fns_);
    int mb = 0;
    if (t && t->vram && ctx_ && t->vram(ctx_, adapterIndex, &mb) == ADL_OK)
    {
        return mb;
    }
    return 0;
}

} // namespace amd
} // namespace idimus_hw

#endif // _WIN32
