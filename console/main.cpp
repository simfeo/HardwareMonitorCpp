// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_monitor — live console hardware monitor. Refreshes CPU/GPU/storage/network every
// N seconds (default 1; pass the interval in seconds as the first argument).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "idimus_hw/idimus_hw.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace idimus_hw;

namespace {

void enableAnsiAndClear() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// Collect readings of one device into channel->value maps, keyed by quantity.
struct DevView {
    const DeviceInfo* info = nullptr;
    std::vector<const Reading*> readings;
};

const Reading* find(const DevView& d, Quantity q, const std::string& chanContains) {
    for (const Reading* r : d.readings)
        if (r->quantity == q && r->channel.find(chanContains) != std::string::npos)
            return r;
    return nullptr;
}

std::string attr(const DeviceInfo& info, const std::string& key) {
    auto it = info.attributes.find(key);
    return it == info.attributes.end() ? std::string() : it->second;
}

double valueOr(const Reading* r, double fallback) { return r ? r->value : fallback; }

std::string fmt(const Reading* r, const char* unit, const char* na = "  n/a") {
    if (!r)
        return na;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%7.1f %s", r->value, unit);
    return buf;
}

std::string humanBytesPerSec(double bps) {
    const char* u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (bps >= 1024.0 && i < 3) {
        bps /= 1024.0;
        ++i;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%6.1f %s", bps, u[i]);
    return buf;
}

void render(const Snapshot& snap) {
    // group readings by device
    std::map<DeviceId, DevView> devs;
    for (const DeviceInfo& d : snap.devices())
        devs[d.id].info = &d;
    for (const Reading& r : snap.readings())
        devs[r.device].readings.push_back(&r);

    std::printf("\033[2J\033[H"); // clear + home
    std::printf("  idimus_hw monitor   (Ctrl+C to quit)\n");
    std::printf("  ------------------------------------------------------------------\n");

    auto kindIs = [](DeviceKind k, std::initializer_list<DeviceKind> ks) {
        for (DeviceKind x : ks)
            if (k == x)
                return true;
        return false;
    };

    for (auto& kv : devs) {
        const DevView& d = kv.second;
        if (!d.info)
            continue;
        DeviceKind k = d.info->id.kind;

        if (k == DeviceKind::Cpu) {
            const Reading* load = find(d, Quantity::Load, "Total");
            const Reading* temp = find(d, Quantity::Temperature, "");
            const Reading* clk = find(d, Quantity::Clock, "Core");
            const Reading* pwr = find(d, Quantity::Power, "Package");
            std::printf("  CPU  %-38.38s\n", d.info->name.c_str());
            std::printf("        load %s   clock %s   temp %s   power %s\n",
                        fmt(load, "%").c_str(), fmt(clk, "MHz").c_str(), fmt(temp, "C").c_str(),
                        fmt(pwr, "W").c_str());
        } else if (kindIs(k, {DeviceKind::GpuIntegrated, DeviceKind::GpuDiscrete})) {
            const Reading* load = find(d, Quantity::Load, "Core");
            const Reading* temp = find(d, Quantity::Temperature, "");
            const Reading* clk = find(d, Quantity::Clock, "Core");
            const Reading* pwr = find(d, Quantity::Power, "");
            std::printf("  GPU  %-38.38s\n", d.info->name.c_str());
            std::printf("        load %s   clock %s   temp %s   power %s\n",
                        fmt(load, "%").c_str(), fmt(clk, "MHz").c_str(), fmt(temp, "C").c_str(),
                        fmt(pwr, "W").c_str());
        } else if (k == DeviceKind::Storage) {
            const Reading* temp = find(d, Quantity::Temperature, "");
            const Reading* used = find(d, Quantity::Level, "Used");
            bool ssd = attr(*d.info, "media") == "SSD";
            std::printf("  DISK %-30.30s used %s   temp %s\n", d.info->name.c_str(),
                        fmt(used, "%").c_str(),
                        ssd ? fmt(temp, "C").c_str() : "  (HDD)"); // temp shown for SSDs only
        } else if (k == DeviceKind::Network) {
            const Reading* dn = find(d, Quantity::DataRate, "Download");
            const Reading* up = find(d, Quantity::DataRate, "Upload");
            // Only show interfaces with current activity to avoid clutter.
            if (valueOr(dn, 0) > 0 || valueOr(up, 0) > 0)
                std::printf("  NET  %-30.30s  v %s   ^ %s\n", d.info->name.c_str(),
                            humanBytesPerSec(valueOr(dn, 0)).c_str(),
                            humanBytesPerSec(valueOr(up, 0)).c_str());
        }
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    double interval = 1.0;
    if (argc > 1) {
        double v = std::atof(argv[1]);
        if (v >= 0.1)
            interval = v;
    }
    enableAnsiAndClear();

    Monitor monitor;
    monitor.addPlatformSources();
    monitor.open();
    monitor.poll(); // prime delta metrics

    auto period = std::chrono::duration<double>(interval);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(period));
        render(monitor.poll());
    }
    return 0;
}
