// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/win_storage_source.hpp"

#ifdef _WIN32

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <winioctl.h>

#include <nvme.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "platform/windows/win_util.hpp"

namespace idimus_hw
{
namespace sources
{
namespace
{

// Read every instance of a wildcard PDH counter as (instanceName, value) pairs.
std::vector<std::pair<std::string, double>> readArray(PDH_HCOUNTER counter)
{
    std::vector<std::pair<std::string, double>> result;
    DWORD bufSize = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &count, nullptr) !=
            PDH_MORE_DATA ||
        bufSize == 0)
    {
        return result;
    }
    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &count, items) !=
        ERROR_SUCCESS)
    {
        return result;
    }
    for (DWORD i = 0; i < count; ++i)
    {
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
        {
            result.emplace_back(win::wideToUtf8(items[i].szName), items[i].FmtValue.doubleValue);
        }
    }
    return result;
}

// PhysicalDisk instance names look like "0 C: D:" — the leading integer is the disk number.
// Returns -1 for "_Total" or anything without a leading digit.
int diskNumberOf(const std::string& instance)
{
    if (instance.empty() || !std::isdigit((unsigned char)instance[0]))
    {
        return -1;
    }
    return std::atoi(instance.c_str());
}

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// IOCTL_STORAGE_QUERY_PROPERTY -> STORAGE_DEVICE_DESCRIPTOR (product name + serial as offsets).
std::string queryProductName(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got,
                         nullptr))
    {
        return {};
    }
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    std::string name;
    if (d->VendorIdOffset && buf[d->VendorIdOffset])
    {
        name += trim(reinterpret_cast<char*>(buf + d->VendorIdOffset)) + " ";
    }
    if (d->ProductIdOffset && buf[d->ProductIdOffset])
    {
        name += trim(reinterpret_cast<char*>(buf + d->ProductIdOffset));
    }
    return trim(name);
}

uint64_t queryGeometry(HANDLE h)
{
    DISK_GEOMETRY_EX g{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof(g), &got,
                        nullptr))
    {
        return uint64_t(g.DiskSize.QuadPart);
    }
    return 0;
}

// Seek-penalty query: SSDs report no seek penalty. Returns 1=SSD, 0=HDD, -1=unknown.
int querySsd(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got,
                        nullptr))
    {
        return d.IncursSeekPenalty ? 0 : 1;
    }
    return -1;
}

// StorageDeviceTemperatureProperty (universal, no elevation). Returns NaN if unavailable.
double queryTemperatureProperty(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got,
                         nullptr))
    {
        return std::nan("");
    }
    auto* d = reinterpret_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    if (d->InfoCount == 0)
    {
        return std::nan("");
    }
    short t = d->TemperatureInfo[0].Temperature; // Celsius
    return (t > -100 && t < 200) ? double(t) : std::nan("");
}

// Fallback for NVMe drives whose driver doesn't answer StorageDeviceTemperatureProperty (e.g.
// Crucial P3 Plus): read the composite temperature from the SMART/Health log page (0x02). The
// value is Kelvin; the log also carries up to 8 per-sensor temperatures. Returns NaN if
// unavailable (e.g. SATA drives, which reject the NVMe protocol query).
double queryTemperatureNvme(HANDLE h)
{
    BYTE buf[sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
             sizeof(NVME_HEALTH_INFO_LOG)] = {};
    auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf);
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    auto* p = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
    p->ProtocolType = ProtocolTypeNvme;
    p->DataType = NVMeDataTypeLogPage;
    p->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO; // 0x02
    p->ProtocolDataRequestSubValue = 0;
    p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);

    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, sizeof(buf), buf, sizeof(buf), &got,
                         nullptr))
    {
        return std::nan("");
    }
    // On return the buffer is a STORAGE_PROTOCOL_DATA_DESCRIPTOR; the log sits at
    // &ProtocolSpecificData + ProtocolDataOffset.
    auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buf);
    auto* pr = &desc->ProtocolSpecificData;
    if (pr->ProtocolDataOffset + pr->ProtocolDataLength > sizeof(buf) - sizeof(STORAGE_PROPERTY_QUERY))
    {
        return std::nan("");
    }
    auto* log =
        reinterpret_cast<NVME_HEALTH_INFO_LOG*>(reinterpret_cast<BYTE*>(pr) + pr->ProtocolDataOffset);
    int kelvin = log->Temperature[0] | (log->Temperature[1] << 8); // composite temp, Kelvin
    double c = double(kelvin) - 273.0;
    return (c > -100 && c < 200 && kelvin != 0) ? c : std::nan("");
}

// Prefer the universal property; fall back to the NVMe health log for drives that don't support it.
double queryTemperature(HANDLE h)
{
    double t = queryTemperatureProperty(h);
    if (std::isnan(t))
    {
        t = queryTemperatureNvme(h);
    }
    return t;
}

// Sum free bytes of fixed volumes, attributed to the physical disk they live on.
std::map<int, uint64_t> freeSpaceByDisk()
{
    std::map<int, uint64_t> result;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(mask & (1u << i)))
        {
            continue;
        }
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
        {
            continue;
        }
        ULARGE_INTEGER freeBytes{};
        if (!GetDiskFreeSpaceExW(root, &freeBytes, nullptr, nullptr))
        {
            continue;
        }

        wchar_t vol[] = {L'\\', L'\\', L'.', L'\\', wchar_t(L'A' + i), L':', 0};
        HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        BYTE buf[512] = {};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf),
                            &got, nullptr))
        {
            auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
            if (ext->NumberOfDiskExtents > 0)
            {
                result[int(ext->Extents[0].DiskNumber)] += freeBytes.QuadPart;
            }
        }
        CloseHandle(h);
    }
    return result;
}

} // namespace

WinStorageSource::~WinStorageSource()
{
    for (auto& d : disks_)
    {
        if (d.handle)
        {
            CloseHandle(static_cast<HANDLE>(d.handle));
        }
    }
    if (query_)
    {
        PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
    }
}

std::vector<DeviceInfo> WinStorageSource::discover()
{
    std::vector<DeviceInfo> result;
    int ordinal = 0;
    for (int n = 0; n < 32; ++n)
    {
        wchar_t path[64];
        wsprintfW(path, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        Disk d;
        d.id = DeviceId{DeviceKind::Storage, ordinal};
        d.number = n;
        d.handle = h;
        d.sizeBytes = queryGeometry(h);
        disks_.push_back(d);

        DeviceInfo info;
        info.id = d.id;
        std::string product = queryProductName(h);
        info.name = product.empty() ? ("PhysicalDrive" + std::to_string(n)) : product;
        info.attributes["physical_drive"] = std::to_string(n);
        int ssd = querySsd(h);
        info.attributes["media"] = ssd == 1 ? "SSD" : ssd == 0 ? "HDD" : "Unknown";
        result.push_back(std::move(info));
        ++ordinal;
    }

    // Per-physical-disk throughput + activity via PDH (English counter names; works unelevated).
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS)
    {
        query_ = q;
        PDH_HCOUNTER c;
        if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", 0, &c) ==
            ERROR_SUCCESS)
        {
            cRead_ = c;
        }
        if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", 0, &c) ==
            ERROR_SUCCESS)
        {
            cWrite_ = c;
        }
        if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\% Idle Time", 0, &c) == ERROR_SUCCESS)
        {
            cIdle_ = c;
        }
        PdhCollectQueryData(q); // prime rate/idle deltas
    }
    return result;
}

void WinStorageSource::sample(std::vector<Reading>& out)
{
    std::map<int, uint64_t> freeMap = freeSpaceByDisk();

    // Collect PDH throughput/activity, keyed by physical-disk number.
    std::map<int, double> readBps, writeBps, idlePct;
    if (query_ && PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) == ERROR_SUCCESS)
    {
        auto fill = [](void* counter, std::map<int, double>& dst)
        {
            if (!counter)
            {
                return;
            }
            for (auto& kv : readArray(static_cast<PDH_HCOUNTER>(counter)))
            {
                int n = diskNumberOf(kv.first);
                if (n >= 0)
                {
                    dst[n] = kv.second;
                }
            }
        };
        fill(cRead_, readBps);
        fill(cWrite_, writeBps);
        fill(cIdle_, idlePct);
    }
    for (auto& d : disks_)
    {
        HANDLE h = static_cast<HANDLE>(d.handle);
        if (d.sizeBytes > 0)
        {
            out.push_back(
                Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        }
        auto fit = freeMap.find(d.number);
        if (fit != freeMap.end())
        {
            out.push_back(
                Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(fit->second)});
            if (d.sizeBytes > 0)
            {
                uint64_t freeB = fit->second > d.sizeBytes ? d.sizeBytes : fit->second;
                out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used",
                                      100.0 * double(d.sizeBytes - freeB) / double(d.sizeBytes)});
            }
        }
        double tC = queryTemperature(h);
        if (!std::isnan(tC))
        {
            out.push_back(Reading{d.id, Quantity::Temperature, Unit::Celsius, "Temperature", tC});
        }

        auto rit = readBps.find(d.number);
        if (rit != readBps.end())
        {
            out.push_back(
                Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Read", rit->second});
        }
        auto wit = writeBps.find(d.number);
        if (wit != writeBps.end())
        {
            out.push_back(
                Reading{d.id, Quantity::DataRate, Unit::BytePerSecond, "Write", wit->second});
        }
        auto iit = idlePct.find(d.number);
        if (iit != idlePct.end())
        {
            double active = 100.0 - iit->second;
            active = active < 0 ? 0 : active > 100 ? 100 : active;
            out.push_back(Reading{d.id, Quantity::Load, Unit::Percent, "Activity", active});
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
