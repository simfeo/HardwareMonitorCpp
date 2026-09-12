// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "platform/windows/cpu_topology.hpp"

#ifdef _WIN32

#include <windows.h>

namespace hardware_monitor_cpp
{
namespace win
{
namespace
{

// Index of the lowest set bit of a group mask, i.e. the first processor of that group affinity.
int firstSetBit(KAFFINITY mask)
{
    for (int i = 0; i < int(sizeof(KAFFINITY) * 8); ++i)
    {
        if (mask & (KAFFINITY(1) << i))
        {
            return i;
        }
    }
    return -1;
}

} // namespace

CpuTopology queryCpuTopology()
{
    CpuTopology t;

    WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; ++g)
    {
        t.groupSizes.push_back(int(GetActiveProcessorCount(g)));
    }
    t.logicalTotal = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (t.logicalTotal <= 0)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        t.logicalTotal = int(si.dwNumberOfProcessors);
        t.groupSizes.assign(1, t.logicalTotal);
    }

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorPackage, nullptr, &len);
    if (len > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        std::vector<BYTE> buf(len);
        auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorPackage, first, &len))
        {
            // Variable-length records: each one carries its own Size, so walk by bytes.
            BYTE* p = buf.data();
            BYTE* end = buf.data() + len;
            while (p < end)
            {
                auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (rec->Size == 0)
                {
                    break;
                }
                if (rec->Relationship == RelationProcessorPackage && rec->Processor.GroupCount > 0)
                {
                    const GROUP_AFFINITY& ga = rec->Processor.GroupMask[0];
                    int bit = firstSetBit(ga.Mask);
                    if (bit >= 0)
                    {
                        ProcessorRef pr;
                        pr.group = uint16_t(ga.Group);
                        pr.number = uint8_t(bit);
                        t.packages.push_back(pr);
                    }
                }
                p += rec->Size;
            }
        }
    }
    if (t.packages.empty())
    {
        t.packages.push_back(ProcessorRef{}); // single package: group 0, processor 0
    }
    return t;
}

ScopedProcessorPin::ScopedProcessorPin(const ProcessorRef& p)
{
    GROUP_AFFINITY want{};
    want.Group = WORD(p.group);
    want.Mask = KAFFINITY(1) << p.number;
    GROUP_AFFINITY previous{};
    pinned_ = SetThreadGroupAffinity(GetCurrentThread(), &want, &previous) != FALSE;
    if (pinned_)
    {
        previousGroup_ = uint16_t(previous.Group);
        previousMask_ = uint64_t(previous.Mask);
    }
}

ScopedProcessorPin::~ScopedProcessorPin()
{
    if (!pinned_)
    {
        return;
    }
    GROUP_AFFINITY restore{};
    restore.Group = WORD(previousGroup_);
    restore.Mask = KAFFINITY(previousMask_);
    SetThreadGroupAffinity(GetCurrentThread(), &restore, nullptr);
}

} // namespace win
} // namespace hardware_monitor_cpp

#endif // _WIN32
