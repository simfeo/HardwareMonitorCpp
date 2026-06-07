// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/battery/linux_battery_source.hpp"

#ifdef __linux__

#include <algorithm>
#include <cstdlib>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw
{
namespace sources
{

std::vector<DeviceInfo> LinuxBatterySource::discover()
{
    std::vector<DeviceInfo> result;
    std::vector<std::string> names = lnx::listDir("/sys/class/power_supply");
    std::sort(names.begin(), names.end());
    int ordinal = 0;
    for (const std::string& n : names)
    {
        std::string path = "/sys/class/power_supply/" + n;
        if (lnx::readTrim(path + "/type") != "Battery")
        {
            continue;
        }
        batteries_.push_back(Bat{DeviceId{DeviceKind::Battery, ordinal}, path});
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::Battery, ordinal};
        std::string model = lnx::readTrim(path + "/model_name");
        info.name = model.empty() ? n : model;
        info.vendor = lnx::readTrim(path + "/manufacturer");
        std::string tech = lnx::readTrim(path + "/technology");
        if (!tech.empty())
        {
            info.attributes["chemistry"] = tech;
        }
        result.push_back(std::move(info));
        ++ordinal;
    }
    return result;
}

void LinuxBatterySource::sample(std::vector<Reading>& out)
{
    for (const Bat& b : batteries_)
    {
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
        { out.push_back(Reading{b.id, q, u, ch, v}); };
        const std::string& p = b.path;
        int64_t v = 0;

        if (lnx::readI64(p + "/capacity", v))
        {
            emit(Quantity::Level, Unit::Percent, "Charge", double(v));
        }

        double voltageV = 0;
        if (lnx::readI64(p + "/voltage_now", v))
        { // µV
            voltageV = v / 1e6;
            emit(Quantity::Voltage, Unit::Volt, "Voltage", voltageV);
        }

        // Capacities: prefer energy_* (µWh); else charge_* (µAh) * voltage.
        auto energyMwh = [&](const char* energyKey, const char* chargeKey, const std::string& ch)
        {
            int64_t e = 0;
            if (lnx::readI64(p + energyKey, e))
            {
                emit(Quantity::Capacity, Unit::MilliwattHour, ch, e / 1000.0); // µWh -> mWh
            }
            else if (lnx::readI64(p + chargeKey, e) && voltageV > 0)
            {
                emit(Quantity::Capacity, Unit::MilliwattHour, ch,
                     (e / 1000.0) * voltageV); // µAh->mAh*V
            }
        };
        energyMwh("/energy_full_design", "/charge_full_design", "Design Capacity");
        energyMwh("/energy_full", "/charge_full", "Full Charge Capacity");
        energyMwh("/energy_now", "/charge_now", "Remaining Capacity");

        if (lnx::readI64(p + "/power_now", v))
        { // µW
            emit(Quantity::Power, Unit::Watt, "Power", std::abs(v) / 1e6);
        }
        else if (lnx::readI64(p + "/current_now", v) && voltageV > 0)
        { // µA
            double a = std::abs(v) / 1e6;
            emit(Quantity::Current, Unit::Ampere, "Current", a);
            emit(Quantity::Power, Unit::Watt, "Power", a * voltageV);
        }

        if (lnx::readI64(p + "/cycle_count", v) && v > 0)
        {
            emit(Quantity::Count, Unit::Count, "Cycle Count", double(v));
        }
        if (lnx::readI64(p + "/temp", v)) // 0.1 °C
        {
            emit(Quantity::Temperature, Unit::Celsius, "Temperature", v / 10.0);
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
