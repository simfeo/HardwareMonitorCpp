// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_hw — the façade: holds the sources, enumerates devices once, and produces snapshots.
#pragma once

#include <memory>
#include <vector>

#include "idimus_hw/device.hpp"
#include "idimus_hw/snapshot.hpp"
#include "idimus_hw/source.hpp"

namespace idimus_hw {

class Monitor {
public:
    Monitor() = default;

    // Adds a source. Ownership is taken.
    void add(std::unique_ptr<Source> source);

    // Populates the default platform sources (createPlatformSources()).
    void addPlatformSources();

    // Discovers devices from all sources. Call once before polling.
    void open();

    // Samples every source and returns an immutable snapshot.
    Snapshot poll();

    const std::vector<DeviceInfo>& devices() const { return devices_; }

private:
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<DeviceInfo> devices_;
    bool opened_ = false;
};

} // namespace idimus_hw
