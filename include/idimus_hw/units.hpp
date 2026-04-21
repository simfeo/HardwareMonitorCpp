// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_hw — physical quantities and units for hardware telemetry readings.
#pragma once

namespace idimus_hw {

// The semantic kind of a measured value. Kept orthogonal to Unit so a consumer can group/format
// readings without parsing names.
enum class Quantity {
    Temperature, // °C
    Load,        // % busy
    Power,       // W
    Voltage,     // V
    Current,     // A
    Clock,       // MHz
    FanSpeed,    // RPM
    Energy,      // stored charge as energy
    Capacity,    // storage/battery capacity
    DataRate,    // throughput
    DataVolume,  // cumulative bytes
    Level,       // 0..100 fill/health
    Duration,    // seconds
    Count,       // dimensionless tally
    Other,
};

enum class Unit {
    Celsius,
    Percent,
    Watt,
    Volt,
    Ampere,
    Megahertz,
    Rpm,
    Joule,
    MilliwattHour,
    Byte,
    BytePerSecond,
    Second,
    Count,
    None,
};

inline const char* unitSymbol(Unit u) {
    switch (u) {
        case Unit::Celsius: return "°C";
        case Unit::Percent: return "%";
        case Unit::Watt: return "W";
        case Unit::Volt: return "V";
        case Unit::Ampere: return "A";
        case Unit::Megahertz: return "MHz";
        case Unit::Rpm: return "RPM";
        case Unit::Joule: return "J";
        case Unit::MilliwattHour: return "mWh";
        case Unit::Byte: return "B";
        case Unit::BytePerSecond: return "B/s";
        case Unit::Second: return "s";
        case Unit::Count: return "";
        case Unit::None: return "";
    }
    return "";
}

} // namespace idimus_hw
