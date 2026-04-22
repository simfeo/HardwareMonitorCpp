// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Storage: whole disks via IOKit (IOMedia) + free space via getmntinfo. macOS only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class MacStorageSource : public Source {
public:
    std::string id() const override { return "macos.storage"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Disk {
        DeviceId id;
        std::string bsdName;
        uint64_t sizeBytes = 0;
    };
    std::vector<Disk> disks_;
};

} // namespace sources
} // namespace idimus_hw
