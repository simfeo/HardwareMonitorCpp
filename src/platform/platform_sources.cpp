// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Assembles the set of telemetry sources for the host platform.
#include "idimus_hw/source.hpp"

#ifdef __APPLE__
#include "sources/cpu/mac_cpu_source.hpp"
#endif

namespace idimus_hw {

std::vector<std::unique_ptr<Source>> createPlatformSources() {
    std::vector<std::unique_ptr<Source>> sources;
#ifdef __APPLE__
    sources.push_back(std::make_unique<sources::MacCpuSource>());
    // GPU, Memory, Network, Storage, Battery sources are appended here as they land.
#endif
    return sources;
}

} // namespace idimus_hw
