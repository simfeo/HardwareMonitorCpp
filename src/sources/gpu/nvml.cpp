// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/nvml.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace idimus_hw
{
namespace nv
{
namespace
{

using Device = void*;
using Ret = int; // nvmlReturn_t; 0 == NVML_SUCCESS

struct Table
{
    Ret (*init)() = nullptr;
    Ret (*shutdown)() = nullptr;
    Ret (*count)(unsigned*) = nullptr;
    Ret (*byIndex)(unsigned, Device*) = nullptr;
    Ret (*name)(Device, char*, unsigned) = nullptr;
    Ret (*temp)(Device, int, unsigned*) = nullptr;
    Ret (*util)(Device, void*) = nullptr;
    Ret (*mem)(Device, void*) = nullptr;
    Ret (*power)(Device, unsigned*) = nullptr;
    Ret (*clock)(Device, int, unsigned*) = nullptr;
    Ret (*fan)(Device, unsigned*) = nullptr;
};

void* openLib()
{
#ifdef _WIN32
    return (void*)LoadLibraryW(L"nvml.dll");
#else
    void* h = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    return h ? h : dlopen("libnvidia-ml.so", RTLD_LAZY);
#endif
}
void* sym(void* lib, const char* n)
{
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, n);
#else
    return dlsym(lib, n);
#endif
}
void closeLib(void* lib)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

// NVML ABI structs (documented layout).
struct NvmlUtil
{
    unsigned gpu, memory;
};
struct NvmlMem
{
    unsigned long long total, free, used;
};

} // namespace

Nvml::Nvml()
{
    lib_ = openLib();
    if (!lib_)
    {
        return;
    }
    auto* t = new Table();
    fns_ = t;
    t->init = (Ret(*)())sym(lib_, "nvmlInit_v2");
    t->shutdown = (Ret(*)())sym(lib_, "nvmlShutdown");
    t->count = (Ret(*)(unsigned*))sym(lib_, "nvmlDeviceGetCount_v2");
    t->byIndex = (Ret(*)(unsigned, Device*))sym(lib_, "nvmlDeviceGetHandleByIndex_v2");
    t->name = (Ret(*)(Device, char*, unsigned))sym(lib_, "nvmlDeviceGetName");
    t->temp = (Ret(*)(Device, int, unsigned*))sym(lib_, "nvmlDeviceGetTemperature");
    t->util = (Ret(*)(Device, void*))sym(lib_, "nvmlDeviceGetUtilizationRates");
    t->mem = (Ret(*)(Device, void*))sym(lib_, "nvmlDeviceGetMemoryInfo");
    t->power = (Ret(*)(Device, unsigned*))sym(lib_, "nvmlDeviceGetPowerUsage");
    t->clock = (Ret(*)(Device, int, unsigned*))sym(lib_, "nvmlDeviceGetClockInfo");
    t->fan = (Ret(*)(Device, unsigned*))sym(lib_, "nvmlDeviceGetFanSpeed");

    if (!t->init || !t->count || !t->byIndex || t->init() != 0)
    {
        return;
    }
    unsigned c = 0;
    if (t->count(&c) != 0)
    {
        return;
    }
    count_ = c;
    ok_ = true;
}

Nvml::~Nvml()
{
    auto* t = static_cast<Table*>(fns_);
    if (ok_ && t && t->shutdown)
    {
        t->shutdown();
    }
    delete t;
    if (lib_)
    {
        closeLib(lib_);
    }
}

void* Nvml::deviceHandle(unsigned index) const
{
    auto* t = static_cast<Table*>(fns_);
    Device d = nullptr;
    return (t && t->byIndex && t->byIndex(index, &d) == 0) ? d : nullptr;
}

std::string Nvml::name(void* dev) const
{
    auto* t = static_cast<Table*>(fns_);
    char buf[96] = {};
    return (t && t->name && t->name(dev, buf, sizeof(buf)) == 0) ? std::string(buf) : std::string();
}

bool Nvml::temperatureC(void* dev, double& out) const
{
    auto* t = static_cast<Table*>(fns_);
    unsigned v = 0;
    if (t && t->temp && t->temp(dev, 0 /*NVML_TEMPERATURE_GPU*/, &v) == 0)
    {
        out = double(v);
        return true;
    }
    return false;
}

bool Nvml::utilization(void* dev, Utilization& out) const
{
    auto* t = static_cast<Table*>(fns_);
    NvmlUtil u{};
    if (t && t->util && t->util(dev, &u) == 0)
    {
        out.gpu = u.gpu;
        out.memory = u.memory;
        return true;
    }
    return false;
}

bool Nvml::memory(void* dev, MemoryInfo& out) const
{
    auto* t = static_cast<Table*>(fns_);
    NvmlMem m{};
    if (t && t->mem && t->mem(dev, &m) == 0)
    {
        out.total = m.total;
        out.free = m.free;
        out.used = m.used;
        return true;
    }
    return false;
}

bool Nvml::powerW(void* dev, double& out) const
{
    auto* t = static_cast<Table*>(fns_);
    unsigned mw = 0;
    if (t && t->power && t->power(dev, &mw) == 0)
    {
        out = mw / 1000.0;
        return true;
    }
    return false;
}

bool Nvml::clockMhz(void* dev, int type, unsigned& out) const
{
    auto* t = static_cast<Table*>(fns_);
    return t && t->clock && t->clock(dev, type, &out) == 0;
}

bool Nvml::fanPercent(void* dev, unsigned& out) const
{
    auto* t = static_cast<Table*>(fns_);
    return t && t->fan && t->fan(dev, &out) == 0;
}

} // namespace nv
} // namespace idimus_hw
