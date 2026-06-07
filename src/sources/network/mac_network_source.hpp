// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Per-interface network counters + throughput via getifaddrs (AF_LINK if_data). macOS only.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw
{
namespace sources
{

class MacNetworkSource : public Source
{
public:
    std::string id() const override
    {
        return "macos.network";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    struct Prev
    {
        uint64_t rx = 0, tx = 0;
        double t = 0;
    };
    std::map<std::string, int> ordinals_; // interface name -> device ordinal
    std::map<std::string, Prev> prev_;
};

} // namespace sources
} // namespace idimus_hw
