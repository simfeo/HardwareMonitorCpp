// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Thin dynamic loader for NVIDIA's NVML (nvml.dll / libnvidia-ml.so). Function signatures are the
// documented public NVML API (facts); no SDK headers required.
#pragma once

#include <cstdint>
#include <string>

namespace idimus_hw
{
namespace nv
{

struct Utilization
{
    unsigned gpu = 0;
    unsigned memory = 0;
};
struct MemoryInfo
{
    uint64_t total = 0, free = 0, used = 0;
};

// Loaded NVML entry points; ok() is false if the driver/library isn't present.
class Nvml
{
public:
    Nvml();
    ~Nvml();
    Nvml(const Nvml&) = delete;
    Nvml& operator=(const Nvml&) = delete;

    bool ok() const
    {
        return ok_;
    }
    unsigned deviceCount() const
    {
        return count_;
    }

    void* deviceHandle(unsigned index) const; // nvmlDevice_t
    std::string name(void* dev) const;
    bool temperatureC(void* dev, double& out) const;
    bool utilization(void* dev, Utilization& out) const;
    bool memory(void* dev, MemoryInfo& out) const;
    bool powerW(void* dev, double& out) const;
    bool clockMhz(void* dev, int type, unsigned& out) const; // 0=graphics,1=sm,2=mem
    bool fanPercent(void* dev, unsigned& out) const;

private:
    bool ok_ = false;
    void* lib_ = nullptr;
    unsigned count_ = 0;
    void* fns_ = nullptr; // opaque table (defined in the .cpp)
};

} // namespace nv
} // namespace idimus_hw
