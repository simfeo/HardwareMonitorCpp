// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Apple Silicon CPU source: per-core/total load (Mach), core temperatures + fans (SMC), package
// and ANE power, and per-cluster active frequency (IOReport). macOS only.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"
#include "platform/macos/ioreport.hpp"
#include "platform/macos/smc.hpp"

namespace idimus_hw {
namespace sources {

class MacCpuSource : public Source {
public:
    MacCpuSource();

    std::string id() const override { return "macos.cpu"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Ticks {
        uint64_t active = 0, idle = 0;
    };

    DeviceId dev_{DeviceKind::Cpu, 0};
    mac::Smc smc_;
    mac::IoReport ioreport_;
    std::vector<std::string> tempKeys_;
    std::vector<std::string> fanKeys_;
    std::vector<Ticks> prevTicks_;
};

} // namespace sources
} // namespace idimus_hw
