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

// Logical processor indices from /sys/devices/system/cpu/cpuN, ascending.
std::vector<int> listLogicalCpus()
{
    std::vector<int> cpus;
    for (const std::string& n : lnx::listDir("/sys/devices/system/cpu"))
    {
        if (n.rfind("cpu", 0) == 0 && n.size() > 3 && isdigit((unsigned char)n[3]))
        {
            cpus.push_back(atoi(n.c_str() + 3));
        }
    }
    std::sort(cpus.begin(), cpus.end());
    return cpus;
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

// coretemp labels its sensors "Package id N", which names the physical package directly. Returns
// -1 when no label says so, leaving the caller to fall back to positional matching.
int hwmonPackageId(const std::string& dir)
{
    for (int i = 1; i <= 32; ++i)
    {
        std::string label = lnx::readTrim(dir + "/temp" + std::to_string(i) + "_label");
        if (label.rfind("Package id ", 0) == 0)
        {
            return atoi(label.c_str() + 11);
        }
    }
    return -1;
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

// A RAPL domain names its socket in `name` as "package-N"; -1 when it does not parse.
int raplPackageId(const std::string& dir)
{
    std::string name = toLower(lnx::readTrim(dir + "/name"));
    return name.rfind("package-", 0) == 0 ? atoi(name.c_str() + 8) : -1;
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

} // namespace

std::vector<DeviceInfo> LinuxCpuSource::discover()
{
    packages_.clear();

    // Group the logical processors by socket. Boards that expose no topology (common on ARM) fall
    // through to a single package holding every CPU, which is what they actually are.
    std::vector<int> cpus = listLogicalCpus();
    for (int cpu : cpus)
    {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                           "/topology/physical_package_id";
        std::string raw = lnx::readTrim(path);
        int pid = raw.empty() ? 0 : atoi(raw.c_str());

        auto it = std::find_if(packages_.begin(), packages_.end(),
                               [pid](const Package& p) { return p.physicalId == pid; });
        if (it == packages_.end())
        {
            Package p;
            p.physicalId = pid;
            packages_.push_back(p);
            it = packages_.end() - 1;
        }
        it->cpus.push_back(cpu);
    }
    if (packages_.empty())
    {
        packages_.push_back(Package{});
    }
    std::sort(packages_.begin(), packages_.end(),
              [](const Package& a, const Package& b) { return a.physicalId < b.physicalId; });
    for (size_t i = 0; i < packages_.size(); ++i)
    {
        packages_[i].dev = DeviceId{DeviceKind::Cpu, int(i)};
    }

    // Temperature sources. Prefer the package id the driver reports; fall back to position when
    // the labels do not carry one (k10temp, SoC hwmons).
    std::vector<std::string> hwmons = findCpuHwmons();
    for (size_t h = 0; h < hwmons.size(); ++h)
    {
        int pid = hwmonPackageId(hwmons[h]);
        auto it = pid >= 0 ? std::find_if(packages_.begin(), packages_.end(),
                                          [pid](const Package& p) { return p.physicalId == pid; })
                           : packages_.end();
        if (it == packages_.end())
        {
            if (h >= packages_.size())
            {
                continue;
            }
            it = packages_.begin() + ptrdiff_t(h);
        }
        it->hwmonDir = hwmons[h];
    }
    if (hwmons.empty())
    {
        packages_.front().thermalZoneDir = findCpuThermalZone();
    }

    std::vector<std::string> raplDirs = findRaplPackages();
    for (size_t r = 0; r < raplDirs.size(); ++r)
    {
        int pid = raplPackageId(raplDirs[r]);
        auto it = pid >= 0 ? std::find_if(packages_.begin(), packages_.end(),
                                          [pid](const Package& p) { return p.physicalId == pid; })
                           : packages_.end();
        if (it == packages_.end())
        {
            if (r >= packages_.size())
            {
                continue;
            }
            it = packages_.begin() + ptrdiff_t(r);
        }
        it->rapl.energyPath = raplDirs[r] + "/energy_uj";
        lnx::readU64(raplDirs[r] + "/max_energy_range_uj", it->rapl.maxRange);
    }

    int nodes = 0;
    for (const std::string& n : lnx::listDir("/sys/devices/system/node"))
    {
        if (n.rfind("node", 0) == 0 && n.size() > 4 && isdigit((unsigned char)n[4]))
        {
            ++nodes;
        }
    }

    std::string model = cpuModelName();
    std::vector<DeviceInfo> devices;
    for (const Package& p : packages_)
    {
        DeviceInfo info;
        info.id = p.dev;
        info.name = model.empty() ? "CPU" : model;
        info.attributes["logical_cores"] = std::to_string(p.cpus.size());
        if (packages_.size() > 1)
        {
            info.attributes["packages"] = std::to_string(packages_.size());
            info.attributes["package_id"] = std::to_string(p.physicalId);
        }
        if (nodes > 1)
        {
            info.attributes["numa_nodes"] = std::to_string(nodes);
        }
        devices.push_back(info);
    }
    return devices;
}

void LinuxCpuSource::sample(std::vector<Reading>& out)
{
    // --- Load (/proc/stat), indexed by logical processor ---
    std::vector<Ticks> cur;
    {
        std::ifstream stat("/proc/stat");
        std::string line;
        while (std::getline(stat, line))
        {
            if (line.rfind("cpu", 0) != 0)
            {
                break;
            }
            std::istringstream ss(line);
            std::string tag;
            ss >> tag;
            if (tag == "cpu")
            {
                continue; // the aggregate row; per-package totals are summed from the cores
            }
            uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0,
                     steal = 0;
            ss >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
            uint64_t idleAll = idle + iowait;
            uint64_t total = user + nice + sys + idleAll + irq + softirq + steal;
            int index = atoi(tag.c_str() + 3);
            if (index >= int(cur.size()))
            {
                cur.resize(size_t(index) + 1);
            }
            cur[size_t(index)] = {idleAll, total};
        }
    }
    bool haveDelta = prev_.size() == cur.size() && !cur.empty();

    for (const Package& pkg : packages_)
    {
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v)
        { out.push_back(Reading{pkg.dev, q, u, ch, v}); };

        if (haveDelta)
        {
            uint64_t sumTotal = 0, sumIdle = 0;
            for (size_t k = 0; k < pkg.cpus.size(); ++k)
            {
                size_t i = size_t(pkg.cpus[k]);
                if (i >= cur.size())
                {
                    continue;
                }
                uint64_t dt = cur[i].total - prev_[i].total;
                uint64_t di = cur[i].idle - prev_[i].idle;
                sumTotal += dt;
                sumIdle += di;
                double pct = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
                emit(Quantity::Load, Unit::Percent, "Core " + std::to_string(k), pct);
            }
            if (sumTotal)
            {
                emit(Quantity::Load, Unit::Percent, "Total",
                     100.0 * double(sumTotal - sumIdle) / double(sumTotal));
            }
        }

        // --- Clock (cpufreq), averaged over this package's cores ---
        double sumMhz = 0, maxMhz = 0;
        int counted = 0;
        for (int cpu : pkg.cpus)
        {
            uint64_t khz = 0;
            if (lnx::readU64("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
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

        // --- Temperature (hwmon, else the thermal-zone fallback) ---
        if (!pkg.hwmonDir.empty())
        {
            double pkgTemp = -1, mx = -1;
            for (int i = 1; i <= 32; ++i)
            {
                std::string base = pkg.hwmonDir + "/temp" + std::to_string(i);
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
                if (label.find("Package") != std::string::npos || label == "Tctl" ||
                    label == "Tdie")
                {
                    pkgTemp = c;
                }
            }
            if (pkgTemp < 0)
            {
                pkgTemp = mx;
            }
            if (pkgTemp > 0)
            {
                emit(Quantity::Temperature, Unit::Celsius, "Package", pkgTemp);
            }
        }
        else if (!pkg.thermalZoneDir.empty())
        {
            int64_t milli = 0;
            if (lnx::readI64(pkg.thermalZoneDir + "/temp", milli))
            {
                double c = milli / 1000.0;
                if (c > 0 && c < 150)
                {
                    emit(Quantity::Temperature, Unit::Celsius, "Thermal Zone", c);
                }
            }
        }
    }

    // --- Package power (RAPL) --- kept out of the loop above because it mutates the wrap state.
    for (Package& pkg : packages_)
    {
        if (pkg.rapl.energyPath.empty())
        {
            continue;
        }
        uint64_t uj = 0;
        if (!lnx::readU64(pkg.rapl.energyPath, uj))
        {
            continue;
        }
        double t = nowSeconds();
        if (pkg.rapl.prevUj >= 0)
        {
            double dE = double(uj) - pkg.rapl.prevUj;
            if (dE < 0 && pkg.rapl.maxRange > 0)
            {
                dE += double(pkg.rapl.maxRange); // counter wrapped
            }
            double dt = t - pkg.rapl.prevTime;
            if (dt > 0 && dE >= 0)
            {
                out.push_back(Reading{pkg.dev, Quantity::Power, Unit::Watt, "Package",
                                      dE / 1e6 / dt});
            }
        }
        pkg.rapl.prevUj = double(uj);
        pkg.rapl.prevTime = t;
    }

    prev_ = std::move(cur);
}

} // namespace sources
} // namespace hardware_monitor_cpp

#endif // __linux__
