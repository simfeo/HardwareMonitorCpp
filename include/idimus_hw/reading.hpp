// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_hw — a single telemetry reading (the atomic unit of this data-oriented model).
#pragma once

#include <string>

#include "idimus_hw/device.hpp"
#include "idimus_hw/units.hpp"

namespace idimus_hw
{

// One measured value attributed to a device channel at sample time. Readings are plain values:
// a snapshot is just a flat list of them, trivially filterable and serializable.
struct Reading
{
    DeviceId device;
    Quantity quantity = Quantity::Other;
    Unit unit = Unit::None;
    std::string channel; // human label within the device: "P-Cluster", "Core 3", "en0 rx", "Tdie"
    double value = 0.0;
};

} // namespace idimus_hw
