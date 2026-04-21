// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_hw — a telemetry source: one data origin (a subsystem on one platform). Sources own any
// state needed for rate/delta metrics and append readings on each sample.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "idimus_hw/device.hpp"
#include "idimus_hw/reading.hpp"

namespace idimus_hw {

class Source {
public:
    virtual ~Source() = default;

    // Stable identifier of the source implementation, e.g. "macos.cpu".
    virtual std::string id() const = 0;

    // Devices this source exposes. Called once by Monitor::open(); may return empty if the source
    // found nothing (e.g. no battery on a desktop).
    virtual std::vector<DeviceInfo> discover() = 0;

    // Append the current readings for this source's devices. Called every Monitor::poll().
    virtual void sample(std::vector<Reading>& out) = 0;
};

// Builds the set of sources appropriate to the host platform (implemented per-OS).
std::vector<std::unique_ptr<Source>> createPlatformSources();

} // namespace idimus_hw
