// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#pragma once

#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class LinuxBatterySource : public Source
{
public:
    std::string id() const override
    {
        return "linux.battery";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Bat
    {
        DeviceId id;
        std::string path; // /sys/class/power_supply/BATx
    };
    std::vector<Bat> batteries_;
};

} // namespace sources
} // namespace hardware_monitor_cpp
