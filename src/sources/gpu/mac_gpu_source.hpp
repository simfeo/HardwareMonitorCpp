// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Apple Silicon integrated GPU: utilization + memory (AGXAccelerator IORegistry), temperature
// (SMC Tg*), power + active frequency (IOReport). macOS only.
#pragma once

#include <string>
#include <vector>

#include "idimus_hw/source.hpp"
#include "platform/macos/ioreport.hpp"
#include "platform/macos/smc.hpp"

namespace idimus_hw {
namespace sources {

class MacGpuSource : public Source {
public:
    MacGpuSource();
    ~MacGpuSource() override;

    std::string id() const override { return "macos.gpu"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    DeviceId dev_{DeviceKind::GpuIntegrated, 0};
    unsigned int service_ = 0; // io_service_t (AGXAccelerator), retained
    int cores_ = 0;
    mac::Smc smc_;
    mac::IoReport ioreport_;
    std::vector<std::string> tempKeys_;
};

} // namespace sources
} // namespace idimus_hw
