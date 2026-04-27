// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Windows CPU: per-core/total load (system processor performance counters) and clock
// (processor power information). Ring-0 temperature/power (PawnIO MSR) is a planned addition.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw {
namespace sources {

class WinCpuSource : public Source {
public:
    std::string id() const override { return "windows.cpu"; }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Ticks {
        uint64_t idle = 0, total = 0;
    };

    DeviceId dev_{DeviceKind::Cpu, 0};
    std::vector<Ticks> prev_;
};

} // namespace sources
} // namespace idimus_hw
