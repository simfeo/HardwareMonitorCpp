// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Minimal demo: enumerate devices and print one snapshot of readings.
#include <chrono>
#include <cstdio>
#include <thread>

#include "idimus_hw/idimus_hw.hpp"

int main() {
    using namespace idimus_hw;

    Monitor monitor;
    monitor.addPlatformSources();
    monitor.open();

    // Prime delta-based metrics (load, power, frequency), then sample over a 1s interval.
    monitor.poll();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Snapshot snap = monitor.poll();

    std::printf("idimus_hw — %zu device(s)\n\n", snap.devices().size());
    for (const DeviceInfo& d : snap.devices()) {
        std::printf("== %s  [%s] ==\n", d.name.c_str(), toString(d.id).c_str());
        for (const auto& a : d.attributes)
            std::printf("   (%s: %s)\n", a.first.c_str(), a.second.c_str());
        for (const Reading& r : snap.forDevice(d.id))
            std::printf("   %-16s %10.2f %s\n", r.channel.c_str(), r.value, unitSymbol(r.unit));
        std::printf("\n");
    }
    return 0;
}
