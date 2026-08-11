// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Battery via IOKit AppleSmartBattery IORegistry properties. macOS only.
#pragma once

#include <string>
#include <vector>

#include "hardware_monitor_cpp/source.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{

class MacBatterySource : public Source
{
public:
    MacBatterySource();
    ~MacBatterySource() override;

    std::string id() const override
    {
        return "macos.battery";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    DeviceId dev_{DeviceKind::Battery, 0};
    unsigned int service_ = 0; // io_service_t (AppleSmartBattery), retained
};

} // namespace sources
} // namespace hardware_monitor_cpp
