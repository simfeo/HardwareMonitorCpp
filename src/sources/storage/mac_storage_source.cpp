// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/mac_storage_source.hpp"

#ifdef __APPLE__

#include <chrono>
#include <sys/mount.h>

#include "platform/macos/cf_util.hpp"

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
// APFS volumes in one container share a free pool and each reports it; take the max, not the sum.
uint64_t freeForDisk(const std::string& bsd)
{
    struct statfs* mounts = nullptr;
    int n = getmntinfo(&mounts, MNT_NOWAIT);
    uint64_t best = 0;
    std::string prefix = "/dev/" + bsd;
    for (int i = 0; i < n; ++i)
    {
        if (std::string(mounts[i].f_mntfromname).rfind(prefix, 0) == 0)
        {
            uint64_t f = uint64_t(mounts[i].f_bavail) * mounts[i].f_bsize;
            if (f > best)
            {
                best = f;
            }
        }
    }
    return best;
}
} // namespace

MacStorageSource::~MacStorageSource()
{
    for (Disk& d : disks_)
    {
        if (d.driver)
        {
            IOObjectRelease(d.driver);
        }
    }
}

std::vector<DeviceInfo> MacStorageSource::discover()
{
    std::vector<DeviceInfo> result;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMedia"), &it) !=
        KERN_SUCCESS)
    {
        return result;
    }

    int ordinal = 0;
    for (io_object_t media = IOIteratorNext(it); media; media = IOIteratorNext(it))
    {
        bool whole = false;
        if (!mac::regBool(media, CFSTR("Whole"), whole) || !whole)
        {
            IOObjectRelease(media);
            continue;
        }
        std::string product =
            mac::searchDictString(media, "Device Characteristics", "Product Name");
        if (product == "Disk Image")
        { // skip mounted DMGs
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
        // The whole-disk IOMedia's service-plane parent is the IOBlockStorageDriver, which
        // publishes a "Statistics" dict (bytes + time read/written). Retain it for sampling.
        io_registry_entry_t parent = 0;
        if (IORegistryEntryGetParentEntry(media, kIOServicePlane, &parent) == KERN_SUCCESS)
        {
            if (IOObjectConformsTo(parent, "IOBlockStorageDriver"))
            {
                d.driver = parent; // keep the +1 reference; released in the destructor
            }
            else
            {
                IOObjectRelease(parent);
            }
        }
        disks_.push_back(d);

        DeviceInfo info;
        info.id = d.id;
        info.name = product.empty() ? (bsd.empty() ? "Disk" : bsd) : product;
        std::string interconnect =
            mac::searchDictString(media, "Protocol Characteristics", "Physical Interconnect");
        if (!interconnect.empty())
        {
            info.attributes["interconnect"] = interconnect;
        }
        info.attributes["bsd_name"] = bsd;
        std::string medium = mac::searchDictString(media, "Device Characteristics", "Medium Type");
        info.attributes["media"] = medium.find("Solid") != std::string::npos        ? "SSD"
                                   : medium.find("Rotational") != std::string::npos ? "HDD"
                                                                                    : "Unknown";
        result.push_back(info);
        IOObjectRelease(media);
    }
    IOObjectRelease(it);
    return result;
}

void MacStorageSource::sample(std::vector<Reading>& out)
{
    for (Disk& d : disks_)
    {
        uint64_t freeB = freeForDisk(d.bsdName);
        out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(freeB)});
        if (d.sizeBytes > 0)
        {
            double usedPct = 100.0 *
                             double(d.sizeBytes - (freeB > d.sizeBytes ? d.sizeBytes : freeB)) /
                             double(d.sizeBytes);
            out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used", usedPct});
        }

        if (!d.driver)
        {
            continue;
        }
        CFTypeRef stats = IORegistryEntryCreateCFProperty(
            (io_registry_entry_t)d.driver, CFSTR("Statistics"), kCFAllocatorDefault, 0);
        if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID())
        {
            auto* dict = (CFDictionaryRef)stats;
            double rb = 0, wb = 0, rt = 0, wt = 0; // bytes + total time (ns), read/write
            mac::dictNumber(dict, "Bytes (Read)", rb);
            mac::dictNumber(dict, "Bytes (Write)", wb);
            mac::dictNumber(dict, "Total Time (Read)", rt);
            mac::dictNumber(dict, "Total Time (Write)", wt);
            double now = nowSeconds();
            if (d.havePrev)
            {
                double dt = now - d.prevT;
                if (dt > 0)
                {
                    double rd = rb >= d.prevReadBytes ? (rb - d.prevReadBytes) / dt : 0.0;
                    double wr = wb >= d.prevWriteBytes ? (wb - d.prevWriteBytes) / dt : 0.0;
                    out.push_back(
                        Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Read", rd});
                    out.push_back(
                        Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Write", wr});
                    double busyNs = (rt - d.prevReadTime) + (wt - d.prevWriteTime);
                    double active = busyNs / (dt * 1e9) * 100.0;
                    active = active < 0 ? 0 : active > 100 ? 100 : active;
                    out.push_back(Reading{d.id, Quantity::Load, Unit::Percent, "Activity", active});
                }
            }
            d.prevReadBytes = rb;
            d.prevWriteBytes = wb;
            d.prevReadTime = rt;
            d.prevWriteTime = wt;
            d.prevT = now;
            d.havePrev = true;
        }
        if (stats)
        {
            CFRelease(stats);
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
