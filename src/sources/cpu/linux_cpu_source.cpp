// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/cpu/linux_cpu_source.hpp"

#ifdef __linux__

#include <chrono>
#include <fstream>
#include <sstream>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw {
namespace sources {
namespace {

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string cpuModelName() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            size_t c = line.find(':');
            if (c != std::string::npos) {
                std::string v = line.substr(c + 1);
                size_t a = v.find_first_not_of(" \t");
                return a == std::string::npos ? std::string() : v.substr(a);
            }
        }
    }
    return {};
}

// Finds the CPU temperature hwmon directory (coretemp / k10temp / zenpower).
std::string findCpuHwmon() {
    for (const std::string& h : lnx::listDir("/sys/class/hwmon")) {
        std::string dir = "/sys/class/hwmon/" + h;
        std::string name = lnx::readTrim(dir + "/name");
        if (name == "coretemp" || name == "k10temp" || name == "zenpower")
            return dir;
    }
    return {};
}

} // namespace

std::vector<DeviceInfo> LinuxCpuSource::discover() {
    DeviceInfo info;
    info.id = dev_;
    std::string name = cpuModelName();
    info.name = name.empty() ? "CPU" : name;

    cores_ = 0;
    for (const std::string& n : lnx::listDir("/sys/devices/system/cpu"))
        if (n.rfind("cpu", 0) == 0 && n.size() > 3 && isdigit((unsigned char)n[3]))
            ++cores_;
    info.attributes["logical_cores"] = std::to_string(cores_);

    hwmonDir_ = findCpuHwmon();
    if (lnx::exists("/sys/class/powercap/intel-rapl:0/energy_uj")) {
        raplEnergyPath_ = "/sys/class/powercap/intel-rapl:0/energy_uj";
        uint64_t range = 0;
        if (lnx::readU64("/sys/class/powercap/intel-rapl:0/max_energy_range_uj", range))
            raplMaxRange_ = range;
    }
    return {info};
}

void LinuxCpuSource::sample(std::vector<Reading>& out) {
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
        out.push_back(Reading{dev_, q, u, ch, v});
    };

    // --- Load (/proc/stat) ---
    std::ifstream stat("/proc/stat");
    std::string line;
    std::vector<Ticks> cur;
    while (std::getline(stat, line)) {
        if (line.rfind("cpu", 0) != 0)
            break;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag; // "cpu" or "cpuN"
        uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        ss >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
        uint64_t idleAll = idle + iowait;
        uint64_t total = user + nice + sys + idleAll + irq + softirq + steal;
        cur.push_back({idleAll, total});
    }
    if (prev_.size() == cur.size() && !cur.empty()) {
        for (size_t i = 0; i < cur.size(); ++i) {
            uint64_t dt = cur[i].total - prev_[i].total;
            uint64_t di = cur[i].idle - prev_[i].idle;
            double pct = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
            if (i == 0)
                emit(Quantity::Load, Unit::Percent, "Total", pct);
            else
                emit(Quantity::Load, Unit::Percent, "Core " + std::to_string(i - 1), pct);
        }
    }
    prev_ = std::move(cur);

    // --- Clock (cpufreq) ---
    double sumMhz = 0, maxMhz = 0;
    int counted = 0;
    for (int i = 0; i < cores_; ++i) {
        uint64_t khz = 0;
        if (lnx::readU64("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                             "/cpufreq/scaling_cur_freq",
                         khz)) {
            double mhz = khz / 1000.0;
            sumMhz += mhz;
            if (mhz > maxMhz)
                maxMhz = mhz;
            ++counted;
        }
    }
    if (counted > 0) {
        emit(Quantity::Clock, Unit::Megahertz, "Core Clock", sumMhz / counted);
        emit(Quantity::Clock, Unit::Megahertz, "Max Clock", maxMhz);
    }

    // --- Temperature (hwmon) ---
    if (!hwmonDir_.empty()) {
        double pkg = -1, mx = -1;
        for (int i = 1; i <= 32; ++i) {
            std::string base = hwmonDir_ + "/temp" + std::to_string(i);
            int64_t milli = 0;
            if (!lnx::readI64(base + "_input", milli))
                continue;
            double c = milli / 1000.0;
            if (c > mx)
                mx = c;
            std::string label = lnx::readTrim(base + "_label");
            if (label.find("Package") != std::string::npos || label == "Tctl" || label == "Tdie")
                pkg = c;
        }
        if (pkg < 0)
            pkg = mx;
        if (pkg > 0)
            emit(Quantity::Temperature, Unit::Celsius, "Package", pkg);
    }

    // --- Package power (RAPL) ---
    if (!raplEnergyPath_.empty()) {
        uint64_t uj = 0;
        if (lnx::readU64(raplEnergyPath_, uj)) {
            double t = nowSeconds();
            if (prevEnergyUj_ >= 0) {
                double dE = double(uj) - prevEnergyUj_;
                if (dE < 0 && raplMaxRange_ > 0)
                    dE += double(raplMaxRange_); // counter wrapped
                double dt = t - prevEnergyTime_;
                if (dt > 0 && dE >= 0)
                    emit(Quantity::Power, Unit::Watt, "Package", dE / 1e6 / dt);
            }
            prevEnergyUj_ = double(uj);
            prevEnergyTime_ = t;
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
