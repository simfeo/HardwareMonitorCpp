// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/linux_storage_source.hpp"

#ifdef __linux__

#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw
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

// /proc/diskstats: per device name -> {sectors read, sectors written, ms doing I/O}.
struct DiskStat
{
    uint64_t rdSectors = 0;
    uint64_t wrSectors = 0;
    uint64_t ioMs = 0;
};
std::map<std::string, DiskStat> readDiskStats()
{
    std::map<std::string, DiskStat> result;
    std::ifstream f("/proc/diskstats");
    std::string line;
    while (std::getline(f, line))
    {
        std::istringstream ss(line);
        unsigned major = 0, minor = 0;
        std::string name;
        uint64_t v[14] = {0}; // fields 4..17 after the name
        if (!(ss >> major >> minor >> name))
        {
            continue;
        }
        int i = 0;
        while (i < 14 && (ss >> v[i]))
        {
            ++i;
        }
        // v[0]=reads, v[2]=sectors read, v[4]=writes, v[6]=sectors written, v[9]=ms doing I/O.
        if (i >= 10)
        {
            result[name] = DiskStat{v[2], v[6], v[9]};
        }
    }
    return result;
}

bool isWholeDisk(const std::string& n)
{
    if (n.rfind("loop", 0) == 0 || n.rfind("ram", 0) == 0 || n.rfind("dm-", 0) == 0 ||
        n.rfind("sr", 0) == 0)
    {
        return false;
    }
    if (n.rfind("nvme", 0) == 0 || n.rfind("mmcblk", 0) == 0)
    {
        return n.find('p') == std::string::npos; // nvme0n1 yes, nvme0n1p1 no
    }
    if (n.rfind("sd", 0) == 0 || n.rfind("hd", 0) == 0 || n.rfind("vd", 0) == 0)
    {
        return !std::isdigit((unsigned char)n.back());
    }
    return false;
}

// Finds a tempN_input under /sys/block/<name>/device/hwmon/* (drivetemp / nvme).
std::string findTempPath(const std::string& name)
{
    std::string base = "/sys/block/" + name + "/device/hwmon";
    for (const std::string& h : lnx::listDir(base))
    {
        std::string p = base + "/" + h + "/temp1_input";
        if (lnx::exists(p))
        {
            return p;
        }
    }
    // NVMe exposes hwmon directly under the device too.
    std::string p2 = "/sys/block/" + name + "/device/device/hwmon";
    for (const std::string& h : lnx::listDir(p2))
    {
        std::string p = p2 + "/" + h + "/temp1_input";
        if (lnx::exists(p))
        {
            return p;
        }
    }
    return {};
}

uint64_t freeForDisk(const std::string& name)
{
    std::ifstream mounts("/proc/mounts");
    std::string dev, mnt, rest;
    uint64_t total = 0;
    const std::string prefix = "/dev/" + name;
    while (mounts >> dev >> mnt)
    {
        std::getline(mounts, rest);
        if (dev.rfind(prefix, 0) == 0)
        {
            struct statvfs st{};
            if (statvfs(mnt.c_str(), &st) == 0)
            {
                total += uint64_t(st.f_bavail) * st.f_frsize;
            }
        }
    }
    return total;
}

} // namespace

std::vector<DeviceInfo> LinuxStorageSource::discover()
{
    std::vector<DeviceInfo> result;
    int ordinal = 0;
    for (const std::string& n : lnx::listDir("/sys/block"))
    {
        if (!isWholeDisk(n))
        {
            continue;
        }
        Disk d;
        d.id = DeviceId{DeviceKind::Storage, ordinal};
        d.name = n;
        uint64_t sectors = 0;
        if (lnx::readU64("/sys/block/" + n + "/size", sectors))
        {
            d.sizeBytes = sectors * 512ull;
        }
        d.tempPath = findTempPath(n);
        disks_.push_back(d);

        DeviceInfo info;
        info.id = d.id;
        std::string model = lnx::readTrim("/sys/block/" + n + "/device/model");
        info.name = model.empty() ? n : model;
        info.attributes["dev"] = "/dev/" + n;
        std::string rot = lnx::readTrim("/sys/block/" + n + "/queue/rotational");
        info.attributes["media"] = rot == "0" ? "SSD" : rot == "1" ? "HDD" : "Unknown";
        result.push_back(std::move(info));
        ++ordinal;
    }
    return result;
}

void LinuxStorageSource::sample(std::vector<Reading>& out)
{
    std::map<std::string, DiskStat> stats = readDiskStats();
    double now = nowSeconds();
    for (const Disk& d : disks_)
    {
        if (d.sizeBytes > 0)
        {
            out.push_back(
                Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        }
        uint64_t freeB = freeForDisk(d.name);
        if (freeB > 0)
        {
            out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(freeB)});
            if (d.sizeBytes > 0)
            {
                uint64_t fb = freeB > d.sizeBytes ? d.sizeBytes : freeB;
                out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used",
                                      100.0 * double(d.sizeBytes - fb) / double(d.sizeBytes)});
            }
        }
        if (!d.tempPath.empty())
        {
            int64_t milli = 0;
            if (lnx::readI64(d.tempPath, milli) && milli > 0)
            {
                out.push_back(Reading{d.id, Quantity::Temperature, Unit::Celsius, "Temperature",
                                      milli / 1000.0});
            }
        }

        auto sit = stats.find(d.name);
        if (sit != stats.end())
        {
            const DiskStat& cur = sit->second;
            auto pit = prev_.find(d.name);
            if (pit != prev_.end())
            {
                double dt = now - pit->second.t;
                if (dt > 0)
                {
                    double rd = cur.rdSectors >= pit->second.rdSectors
                                    ? double(cur.rdSectors - pit->second.rdSectors) * 512.0 / dt
                                    : 0.0;
                    double wr = cur.wrSectors >= pit->second.wrSectors
                                    ? double(cur.wrSectors - pit->second.wrSectors) * 512.0 / dt
                                    : 0.0;
                    out.push_back(
                        Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Read", rd});
                    out.push_back(
                        Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Write", wr});
                    if (cur.ioMs >= pit->second.ioMs)
                    {
                        double active = double(cur.ioMs - pit->second.ioMs) / (dt * 1000.0) * 100.0;
                        active = active < 0 ? 0 : active > 100 ? 100 : active;
                        out.push_back(
                            Reading{d.id, Quantity::Load, Unit::Percent, "Activity", active});
                    }
                }
            }
            prev_[d.name] = Prev{cur.rdSectors, cur.wrSectors, cur.ioMs, now};
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
