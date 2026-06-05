// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/linux_storage_source.hpp"

#ifdef __linux__

#include <cctype>
#include <fstream>
#include <sys/statvfs.h>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw {
namespace sources {
namespace {

bool isWholeDisk(const std::string& n) {
    if (n.rfind("loop", 0) == 0 || n.rfind("ram", 0) == 0 || n.rfind("dm-", 0) == 0 ||
        n.rfind("sr", 0) == 0)
        return false;
    if (n.rfind("nvme", 0) == 0 || n.rfind("mmcblk", 0) == 0)
        return n.find('p') == std::string::npos; // nvme0n1 yes, nvme0n1p1 no
    if (n.rfind("sd", 0) == 0 || n.rfind("hd", 0) == 0 || n.rfind("vd", 0) == 0)
        return !std::isdigit((unsigned char)n.back());
    return false;
}

// Finds a tempN_input under /sys/block/<name>/device/hwmon/* (drivetemp / nvme).
std::string findTempPath(const std::string& name) {
    std::string base = "/sys/block/" + name + "/device/hwmon";
    for (const std::string& h : lnx::listDir(base)) {
        std::string p = base + "/" + h + "/temp1_input";
        if (lnx::exists(p))
            return p;
    }
    // NVMe exposes hwmon directly under the device too.
    std::string p2 = "/sys/block/" + name + "/device/device/hwmon";
    for (const std::string& h : lnx::listDir(p2)) {
        std::string p = p2 + "/" + h + "/temp1_input";
        if (lnx::exists(p))
            return p;
    }
    return {};
}

uint64_t freeForDisk(const std::string& name) {
    std::ifstream mounts("/proc/mounts");
    std::string dev, mnt, rest;
    uint64_t total = 0;
    const std::string prefix = "/dev/" + name;
    while (mounts >> dev >> mnt) {
        std::getline(mounts, rest);
        if (dev.rfind(prefix, 0) == 0) {
            struct statvfs st {};
            if (statvfs(mnt.c_str(), &st) == 0)
                total += uint64_t(st.f_bavail) * st.f_frsize;
        }
    }
    return total;
}

} // namespace

std::vector<DeviceInfo> LinuxStorageSource::discover() {
    std::vector<DeviceInfo> result;
    int ordinal = 0;
    for (const std::string& n : lnx::listDir("/sys/block")) {
        if (!isWholeDisk(n))
            continue;
        Disk d;
        d.id = DeviceId{DeviceKind::Storage, ordinal};
        d.name = n;
        uint64_t sectors = 0;
        if (lnx::readU64("/sys/block/" + n + "/size", sectors))
            d.sizeBytes = sectors * 512ull;
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

void LinuxStorageSource::sample(std::vector<Reading>& out) {
    for (const Disk& d : disks_) {
        if (d.sizeBytes > 0)
            out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        uint64_t freeB = freeForDisk(d.name);
        if (freeB > 0) {
            out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(freeB)});
            if (d.sizeBytes > 0) {
                uint64_t fb = freeB > d.sizeBytes ? d.sizeBytes : freeB;
                out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used",
                                      100.0 * double(d.sizeBytes - fb) / double(d.sizeBytes)});
            }
        }
        if (!d.tempPath.empty()) {
            int64_t milli = 0;
            if (lnx::readI64(d.tempPath, milli) && milli > 0)
                out.push_back(Reading{d.id, Quantity::Temperature, Unit::Celsius, "Temperature",
                                      milli / 1000.0});
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
