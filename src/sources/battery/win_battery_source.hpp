// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Windows battery / UPS via the battery device interface (SetupAPI + IOCTL_BATTERY_*).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class WinBatterySource : public Source {
public:
    ~WinBatterySource() override;

    std::string id() const override { return "windows.battery"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Bat {
        DeviceId id;
        void* handle = nullptr; // Win32 HANDLE
        uint32_t tag = 0;
        uint32_t designCapacity = 0;     // mWh (0xFFFFFFFF = unknown)
        uint32_t fullChargeCapacity = 0; // mWh
    };
    std::vector<Bat> batteries_;
};

} // namespace sources
} // namespace idimus_hw
