// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/mac_storage_source.hpp"

#ifdef __APPLE__

#include <sys/mount.h>

#include "platform/macos/cf_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {
// APFS volumes in one container share a free pool and each reports it; take the max, not the sum.
uint64_t freeForDisk(const std::string& bsd) {
    struct statfs* mounts = nullptr;
    int n = getmntinfo(&mounts, MNT_NOWAIT);
    uint64_t best = 0;
    std::string prefix = "/dev/" + bsd;
    for (int i = 0; i < n; ++i) {
        if (std::string(mounts[i].f_mntfromname).rfind(prefix, 0) == 0) {
            uint64_t f = uint64_t(mounts[i].f_bavail) * mounts[i].f_bsize;
            if (f > best)
                best = f;
        }
    }
    return best;
}
} // namespace

std::vector<DeviceInfo> MacStorageSource::discover() {
    std::vector<DeviceInfo> result;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMedia"), &it) !=
        KERN_SUCCESS)
        return result;

    int ordinal = 0;
    for (io_object_t media = IOIteratorNext(it); media; media = IOIteratorNext(it)) {
        bool whole = false;
        if (!mac::regBool(media, CFSTR("Whole"), whole) || !whole) {
            IOObjectRelease(media);
            continue;
        }
        std::string product = mac::searchDictString(media, "Device Characteristics", "Product Name");
        if (product == "Disk Image") { // skip mounted DMGs
            IOObjectRelease(media);
            continue;
        }
        std::string bsd = mac::regString(media, CFSTR("BSD Name"));
        long long size = 0;
        mac::regNumber(media, CFSTR("Size"), size);

        Disk d;
        d.id = DeviceId{DeviceKind::Storage, ordinal++};
        d.bsdName = bsd;
        d.sizeBytes = uint64_t(size);
        disks_.push_back(d);

        DeviceInfo info;
        info.id = d.id;
        info.name = product.empty() ? (bsd.empty() ? "Disk" : bsd) : product;
        std::string interconnect =
            mac::searchDictString(media, "Protocol Characteristics", "Physical Interconnect");
        if (!interconnect.empty())
            info.attributes["interconnect"] = interconnect;
        info.attributes["bsd_name"] = bsd;
        std::string medium = mac::searchDictString(media, "Device Characteristics", "Medium Type");
        info.attributes["media"] = medium.find("Solid") != std::string::npos ? "SSD"
                                   : medium.find("Rotational") != std::string::npos ? "HDD"
                                                                                    : "Unknown";
        result.push_back(info);
        IOObjectRelease(media);
    }
    IOObjectRelease(it);
    return result;
}

void MacStorageSource::sample(std::vector<Reading>& out) {
    for (const Disk& d : disks_) {
        uint64_t freeB = freeForDisk(d.bsdName);
        out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(freeB)});
        if (d.sizeBytes > 0) {
            double usedPct = 100.0 * double(d.sizeBytes - (freeB > d.sizeBytes ? d.sizeBytes : freeB)) /
                             double(d.sizeBytes);
            out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used", usedPct});
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
