// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// System memory: physical used/available/load + swap. macOS only.
#pragma once

#include <string>
#include <vector>

#include "idimus_hw/source.hpp"

namespace idimus_hw
{
namespace sources
{

class MacMemorySource : public Source
{
public:
    std::string id() const override
    {
        return "macos.memory";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    DeviceId dev_{DeviceKind::Memory, 0};
};

} // namespace sources
} // namespace idimus_hw
