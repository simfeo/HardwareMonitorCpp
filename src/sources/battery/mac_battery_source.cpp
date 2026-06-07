// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/battery/mac_battery_source.hpp"

#ifdef __APPLE__

#include <cmath>

#include "platform/macos/cf_util.hpp"

namespace idimus_hw
{
namespace sources
{

MacBatterySource::MacBatterySource()
{
    service_ =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
}

MacBatterySource::~MacBatterySource()
{
    if (service_)
    {
        IOObjectRelease(service_);
    }
}

std::vector<DeviceInfo> MacBatterySource::discover()
{
    if (!service_)
    {
        return {}; // no built-in battery (e.g. desktop)
    }
    DeviceInfo info;
    info.id = dev_;
    info.name = "Battery";
    info.vendor = "Apple";
    info.attributes["chemistry"] = "Lithium Ion";
    return {info};
}

void MacBatterySource::sample(std::vector<Reading>& out)
{
    if (!service_)
    {
        return;
    }
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };
    auto num = [&](const char* key, long long& v)
    {
        CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
        bool ok = mac::regNumber(service_, k, v);
        CFRelease(k);
        return ok;
    };

    long long v = 0;
    double voltageV = 0;
    if (num("Voltage", v))
    {
        voltageV = v / 1000.0;
        emit(Quantity::Voltage, Unit::Volt, "Voltage", voltageV);
    }

    long long rawCur = 0, rawMax = 0, designMah = 0;
    bool haveCur = num("AppleRawCurrentCapacity", rawCur);
    bool haveMax = num("AppleRawMaxCapacity", rawMax);
    num("DesignCapacity", designMah);

    // Energy (mWh) = capacity (mAh) * voltage (V).
    if (designMah > 0 && voltageV > 0)
    {
        emit(Quantity::Capacity, Unit::MilliwattHour, "Design Capacity", designMah * voltageV);
    }
    if (haveMax && voltageV > 0)
    {
        emit(Quantity::Capacity, Unit::MilliwattHour, "Full Charge Capacity", rawMax * voltageV);
    }
    if (haveCur && voltageV > 0)
    {
        emit(Quantity::Capacity, Unit::MilliwattHour, "Remaining Capacity", rawCur * voltageV);
    }

    if (haveCur && haveMax && rawMax > 0)
    {
        emit(Quantity::Level, Unit::Percent, "Charge", 100.0 * double(rawCur) / double(rawMax));
    }
    if (designMah > 0 && haveMax)
    {
        emit(Quantity::Level, Unit::Percent, "Health", 100.0 * double(rawMax) / double(designMah));
    }

    long long amps = 0;
    bool haveAmp = num("Amperage", amps);
    if (haveAmp)
    {
        double currentA = amps / 1000.0; // signed: negative = discharging
        emit(Quantity::Current, Unit::Ampere, "Current", std::fabs(currentA));
        emit(Quantity::Power, Unit::Watt, "Power", std::fabs(currentA * voltageV));
    }

    if (num("Temperature", v))
    {
        emit(Quantity::Temperature, Unit::Celsius, "Temperature", v / 100.0);
    }
    if (num("CycleCount", v))
    {
        emit(Quantity::Count, Unit::Count, "Cycle Count", double(v));
    }

    long long charging = 0, minutes = 0;
    bool isCharging = num("IsCharging", charging) && charging != 0;
    bool haveTime = isCharging ? num("AvgTimeToFull", minutes) : num("AvgTimeToEmpty", minutes);
    if (haveTime && minutes > 0 && minutes < 65535)
    {
        emit(Quantity::Duration, Unit::Second, isCharging ? "Time To Full" : "Time To Empty",
             double(minutes * 60));
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
