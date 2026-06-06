// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Storage: whole disks via IOKit (IOMedia) + free space via getmntinfo. macOS only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw
{
namespace sources
{

class MacStorageSource : public Source
{
public:
    ~MacStorageSource() override;

    std::string id() const override
    {
        return "macos.storage";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Disk
    {
        DeviceId id;
        std::string bsdName;
        uint64_t sizeBytes = 0;
        unsigned int driver = 0; // IOBlockStorageDriver io_object_t (retained); 0 if none
        // Previous IOBlockStorageDriver "Statistics" counters for throughput + activity deltas.
        bool havePrev = false;
        double prevReadBytes = 0, prevWriteBytes = 0, prevReadTime = 0, prevWriteTime = 0,
               prevT = 0;
    };
    std::vector<Disk> disks_;
};

} // namespace sources
} // namespace idimus_hw
