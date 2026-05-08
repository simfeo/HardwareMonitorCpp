// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Windows CPU: per-core/total load (system processor performance counters), clock (processor
// power information), and — on Intel with PawnIO present — package temperature (digital thermal
// sensor) and RAPL package/cores/uncore power.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"
#include "platform/windows/pawnio.hpp"

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
    // RAPL energy counter state for one domain (32-bit counter, wraps).
    struct Energy {
        uint32_t last = 0;
        double t = 0;
        bool primed = false;
    };

    enum class Vendor { Other, Intel, Amd };

    void readRapl(std::vector<Reading>& out, uint32_t msr, Energy& st, const char* channel,
                  double energyJoule);

    DeviceId dev_{DeviceKind::Cpu, 0};
    std::vector<Ticks> prev_;

    win::PawnIo pawn_;
    Vendor vendor_ = Vendor::Other;
    bool msr_ = false;          // ring-0 path active (module loaded + units read)
    double tjMax_ = 100.0;      // Intel
    double energyJoule_ = 0.0;  // joules per RAPL energy tick (Intel or AMD)
    Energy ePkg_, ePp0_, ePp1_; // Intel package/cores/uncore
    Energy amdPkg_;             // AMD package energy
};

} // namespace sources
} // namespace idimus_hw
