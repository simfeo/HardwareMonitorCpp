// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Intel discrete GPU (Arc) telemetry via IGCL. Windows. UNVERIFIED on Intel Arc hardware.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "idimus_hw/source.hpp"
#include "sources/gpu/igcl.hpp"

namespace idimus_hw
{
namespace sources
{

class IntelGpuSource : public Source
{
public:
    std::string id() const override
    {
        return "windows.gpu.intel_arc";
    }
    std::vector<DeviceInfo> discover() override;
    void sample(std::vector<Reading>& out) override;

private:
    std::unique_ptr<intel::Igcl> igcl_;
    size_t count_ = 0;
};

} // namespace sources
} // namespace idimus_hw
