// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Processor-group and package topology. Windows splits machines with more than 64 logical
// processors into groups, and the classic APIs (GetSystemInfo, affinity masks) only ever see the
// caller's group - so without this the CPU source silently reports half a dual-socket box.
//
// The data types stay free of Win32 types because win_cpu_source.hpp holds them by value and is
// compiled on every platform; only the implementation side is Windows-gated.
#pragma once

#include <cstdint>
#include <vector>

namespace hardware_monitor_cpp
{
namespace win
{

struct ProcessorRef
{
    uint16_t group = 0;
    uint8_t number = 0;
};

struct PackageInfo
{
    std::vector<ProcessorRef> processors; // every logical processor of this package
    // Indices into the flat, group-ordered processor array that collectPerf() fills.
    std::vector<int> flatIndices;
};

struct CpuTopology
{
    int logicalTotal = 0;              // active processors across every group
    std::vector<int> groupSizes;       // active processors in each group
    std::vector<PackageInfo> packages; // one entry per physical package
};

#ifdef _WIN32

CpuTopology queryCpuTopology();

// Pins the calling thread to one processor for the duration of the scope, so a ring-0 MSR read
// lands on the intended core/socket. Restores the previous affinity on destruction.
class ScopedProcessorPin
{
public:
    explicit ScopedProcessorPin(const ProcessorRef& p);
    ~ScopedProcessorPin();
    ScopedProcessorPin(const ScopedProcessorPin&) = delete;
    ScopedProcessorPin& operator=(const ScopedProcessorPin&) = delete;

    bool ok() const
    {
        return pinned_;
    }

private:
    uint64_t previousMask_ = 0;
    uint16_t previousGroup_ = 0;
    bool pinned_ = false;
};

#endif // _WIN32

} // namespace win
} // namespace hardware_monitor_cpp
