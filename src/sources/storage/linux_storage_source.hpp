// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw
{
namespace sources
{

class LinuxStorageSource : public Source
{
public:
    std::string id() const override
    {
        return "linux.storage";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Disk
    {
        DeviceId id;
        std::string name; // e.g. "nvme0n1"
        uint64_t sizeBytes = 0;
        std::string tempPath; // hwmon tempN_input (if found)
    };
    std::vector<Disk> disks_;

    // Previous /proc/diskstats counters for throughput + activity deltas.
    struct Prev
    {
        uint64_t rdSectors = 0;
        uint64_t wrSectors = 0;
        uint64_t ioMs = 0;
        double t = 0;
    };
    std::map<std::string, Prev> prev_;
};

} // namespace sources
} // namespace idimus_hw
