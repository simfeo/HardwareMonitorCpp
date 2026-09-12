// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/cpu/linux_cpu_source.hpp"

#ifdef __linux__

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "platform/linux/sysfs.hpp"

namespace hardware_monitor_cpp
{
namespace sources
{
namespace
{

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string cpuModelName()
{
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("model name", 0) == 0)
        {
            size_t c = line.find(':');
            if (c != std::string::npos)
            {
                std::string v = line.substr(c + 1);
                size_t a = v.find_first_not_of(" \t");
                return a == std::string::npos ? std::string() : v.substr(a);
            }
        }
    }
    return {};
}

std::string toLower(std::string s)
{
    for (char& c : s)
    {
        c = char(tolower((unsigned char)c));
    }
    return s;
}

// Trailing decimal of a sysfs entry name ("hwmon3" -> 3, "intel-rapl:1" -> 1); -1 if absent.
int trailingIndex(const std::string& name)
{
    size_t i = name.size();
    while (i > 0 && isdigit((unsigned char)name[i - 1]))
    {
        --i;
    }
    return i == name.size() ? -1 : atoi(name.c_str() + i);
}

// Finds the CPU temperature hwmon directories. The x86 drivers register one hwmon per physical
// package, so a multi-socket box yields several and all of them are kept. The second pass catches
// ARM/SoC boards, where the CPU thermal zone surfaces as an hwmon with a board-specific name
// (cpu_thermal, soc_thermal, scmi/scpi sensor providers, ...) and is inherently single.
std::vector<std::string> findCpuHwmons()
{
    std::vector<std::string> entries = lnx::listDir("/sys/class/hwmon");
    std::sort(entries.begin(), entries.end(),
              [](const std::string& a, const std::string& b)
              { return trailingIndex(a) < trailingIndex(b); });

    std::vector<std::string> found;
    for (const std::string& h : entries)
    {
        std::string dir = "/sys/class/hwmon/" + h;
        std::string name = lnx::readTrim(dir + "/name");
        if (name == "coretemp" || name == "k10temp" || name == "zenpower")
        {
            found.push_back(dir);
        }
    }
    if (!found.empty())
    {
        return found;
    }
    for (const std::string& h : entries)
    {
        std::string dir = "/sys/class/hwmon/" + h;
        std::string name = toLower(lnx::readTrim(dir + "/name"));
        if (name.find("cpu") != std::string::npos || name == "soc_thermal" ||
            name == "scmi_sensors" || name == "scpi_sensors")
        {
            found.push_back(dir);
            break;
        }
    }
    return found;
}

// Top-level RAPL package domains, one per socket: "intel-rapl:N". Subdomains ("intel-rapl:0:0" =
// cores/uncore) are skipped - their energy is already counted inside the package domain.
std::vector<std::string> findRaplPackages()
{
    std::vector<std::string> entries;
    for (const std::string& e : lnx::listDir("/sys/class/powercap"))
    {
        if (e.rfind("intel-rapl:", 0) != 0 || e.find(':', 11) != std::string::npos)
        {
            continue;
        }
        std::string dir = "/sys/class/powercap/" + e;
        if (!lnx::exists(dir + "/energy_uj"))
        {
            continue;
        }
        if (toLower(lnx::readTrim(dir + "/name")).rfind("package", 0) != 0)
        {
            continue;
        }
        entries.push_back(dir);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::string& a, const std::string& b)
              { return trailingIndex(a) < trailingIndex(b); });
    return entries;
}

// Fallback for boards that expose the CPU thermal zone only through the thermal framework and
// register no hwmon for it. Returns a thermal_zoneN directory whose `type` names the CPU.
std::string findCpuThermalZone()
{
    for (const std::string& z : lnx::listDir("/sys/class/thermal"))
    {
        if (z.rfind("thermal_zone", 0) != 0)
        {
            continue;
        }
        std::string dir = "/sys/class/thermal/" + z;
        std::string type = toLower(lnx::readTrim(dir + "/type"));
        if (type.find("cpu") != std::string::npos || type.find("soc") != std::string::npos)
        {
            return dir;
        }
    }
    return {};
}

// Single-socket machines keep the plain "Package" channel they always had; only multi-socket
// boxes get numbered channels, so existing consumers see no change.
std::string packageChannel(size_t index, size_t total)
{
    return total > 1 ? "Package " + std::to_string(index) : std::string("Package");
}

} // namespace

std::vector<DeviceInfo> LinuxCpuSource::discover()
{
    DeviceInfo info;
    info.id = dev_;
    std::string name = cpuModelName();
    info.name = name.empty() ? "CPU" : name;

    cores_ = 0;
    for (const std::string& n : lnx::listDir("/sys/devices/system/cpu"))
    {
        if (n.rfind("cpu", 0) == 0 && n.size() > 3 && isdigit((unsigned char)n[3]))
        {
            ++cores_;
        }
    }
    info.attributes["logical_cores"] = std::to_string(cores_);

    int nodes = 0;
    for (const std::string& n : lnx::listDir("/sys/devices/system/node"))
    {
        if (n.rfind("node", 0) == 0 && n.size() > 4 && isdigit((unsigned char)n[4]))
        {
            ++nodes;
        }
    }
    if (nodes > 1)
    {
        info.attributes["numa_nodes"] = std::to_string(nodes);
    }

    hwmonDirs_ = findCpuHwmons();
    if (hwmonDirs_.empty())
    {
        thermalZoneDir_ = findCpuThermalZone();
    }
    if (hwmonDirs_.size() > 1)
    {
        info.attributes["packages"] = std::to_string(hwmonDirs_.size());
    }

    for (const std::string& dir : findRaplPackages())
    {
        RaplDomain d;
        d.energyPath = dir + "/energy_uj";
        lnx::readU64(dir + "/max_energy_range_uj", d.maxRange);
        rapl_.push_back(d);
    }
    return {info};
}

void LinuxCpuSource::sample(std::vector<Reading>& out)
{
    auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
    { out.push_back(Reading{dev_, q, u, ch, v}); };

    // --- Load (/proc/stat) ---
    std::ifstream stat("/proc/stat");
    std::string line;
    std::vector<Ticks> cur;
    while (std::getline(stat, line))
    {
        if (line.rfind("cpu", 0) != 0)
        {
            break;
        }
        std::istringstream ss(line);
        std::string tag;
        ss >> tag; // "cpu" or "cpuN"
        uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        ss >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
        uint64_t idleAll = idle + iowait;
        uint64_t total = user + nice + sys + idleAll + irq + softirq + steal;
        cur.push_back({idleAll, total});
    }
    if (prev_.size() == cur.size() && !cur.empty())
    {
        for (size_t i = 0; i < cur.size(); ++i)
        {
            uint64_t dt = cur[i].total - prev_[i].total;
            uint64_t di = cur[i].idle - prev_[i].idle;
            double pct = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
            if (i == 0)
            {
                emit(Quantity::Load, Unit::Percent, "Total", pct);
            }
            else
            {
                emit(Quantity::Load, Unit::Percent, "Core " + std::to_string(i - 1), pct);
            }
        }
    }
    prev_ = std::move(cur);

    // --- Clock (cpufreq) ---
    double sumMhz = 0, maxMhz = 0;
    int counted = 0;
    for (int i = 0; i < cores_; ++i)
    {
        uint64_t khz = 0;
        if (lnx::readU64("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                             "/cpufreq/scaling_cur_freq",
                         khz))
        {
            double mhz = khz / 1000.0;
            sumMhz += mhz;
            if (mhz > maxMhz)
            {
                maxMhz = mhz;
            }
            ++counted;
        }
    }
    if (counted > 0)
    {
        emit(Quantity::Clock, Unit::Megahertz, "Core Clock", sumMhz / counted);
        emit(Quantity::Clock, Unit::Megahertz, "Max Clock", maxMhz);
    }

    // --- Temperature (hwmon, one per physical package) ---
    for (size_t h = 0; h < hwmonDirs_.size(); ++h)
    {
        double pkg = -1, mx = -1;
        for (int i = 1; i <= 32; ++i)
        {
            std::string base = hwmonDirs_[h] + "/temp" + std::to_string(i);
            int64_t milli = 0;
            if (!lnx::readI64(base + "_input", milli))
            {
                continue;
            }
            double c = milli / 1000.0;
            if (c > mx)
            {
                mx = c;
            }
            std::string label = lnx::readTrim(base + "_label");
            if (label.find("Package") != std::string::npos || label == "Tctl" || label == "Tdie")
            {
                pkg = c;
            }
        }
        if (pkg < 0)
        {
            pkg = mx;
        }
        if (pkg > 0)
        {
            emit(Quantity::Temperature, Unit::Celsius, packageChannel(h, hwmonDirs_.size()), pkg);
        }
    }
    if (hwmonDirs_.empty() && !thermalZoneDir_.empty())
    {
        int64_t milli = 0;
        if (lnx::readI64(thermalZoneDir_ + "/temp", milli))
        {
            double c = milli / 1000.0;
            if (c > 0 && c < 150)
            {
                emit(Quantity::Temperature, Unit::Celsius, "Thermal Zone", c);
            }
        }
    }

    // --- Package power (RAPL, one domain per socket) ---
    for (size_t r = 0; r < rapl_.size(); ++r)
    {
        RaplDomain& d = rapl_[r];
        uint64_t uj = 0;
        if (!lnx::readU64(d.energyPath, uj))
        {
            continue;
        }
        double t = nowSeconds();
        if (d.prevUj >= 0)
        {
            double dE = double(uj) - d.prevUj;
            if (dE < 0 && d.maxRange > 0)
            {
                dE += double(d.maxRange); // counter wrapped
            }
            double dt = t - d.prevTime;
            if (dt > 0 && dE >= 0)
            {
                emit(Quantity::Power, Unit::Watt, packageChannel(r, rapl_.size()), dE / 1e6 / dt);
            }
        }
        d.prevUj = double(uj);
        d.prevTime = t;
    }
}

} // namespace sources
} // namespace hardware_monitor_cpp

#endif // __linux__
