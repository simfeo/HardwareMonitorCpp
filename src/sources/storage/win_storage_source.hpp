// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Windows storage: physical drives via IOCTL_STORAGE_QUERY_PROPERTY (identity, temperature) +
// geometry (size) + per-disk free space (volume disk extents).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class WinStorageSource : public Source {
public:
    ~WinStorageSource() override;

    std::string id() const override { return "windows.storage"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Disk {
        DeviceId id;
        int number = -1;
        void* handle = nullptr; // Win32 HANDLE (kept open for temperature queries)
        uint64_t sizeBytes = 0;
    };
    std::vector<Disk> disks_;
};

} // namespace sources
} // namespace idimus_hw
