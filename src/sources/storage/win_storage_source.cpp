// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/storage/win_storage_source.hpp"

#ifdef _WIN32

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <winioctl.h>

namespace idimus_hw {
namespace sources {
namespace {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// IOCTL_STORAGE_QUERY_PROPERTY -> STORAGE_DEVICE_DESCRIPTOR (product name + serial as offsets).
std::string queryProductName(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got,
                         nullptr))
        return {};
    auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    std::string name;
    if (d->VendorIdOffset && buf[d->VendorIdOffset])
        name += trim(reinterpret_cast<char*>(buf + d->VendorIdOffset)) + " ";
    if (d->ProductIdOffset && buf[d->ProductIdOffset])
        name += trim(reinterpret_cast<char*>(buf + d->ProductIdOffset));
    return trim(name);
}

uint64_t queryGeometry(HANDLE h) {
    DISK_GEOMETRY_EX g{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &g, sizeof(g), &got,
                        nullptr))
        return uint64_t(g.DiskSize.QuadPart);
    return 0;
}

// Seek-penalty query: SSDs report no seek penalty. Returns 1=SSD, 0=HDD, -1=unknown.
int querySsd(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got, nullptr))
        return d.IncursSeekPenalty ? 0 : 1;
    return -1;
}

// StorageDeviceTemperatureProperty (universal, no elevation). Returns NaN if unavailable.
double queryTemperature(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got,
                         nullptr))
        return std::nan("");
    auto* d = reinterpret_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    if (d->InfoCount == 0)
        return std::nan("");
    short t = d->TemperatureInfo[0].Temperature; // Celsius
    return (t > -100 && t < 200) ? double(t) : std::nan("");
}

// Sum free bytes of fixed volumes, attributed to the physical disk they live on.
std::map<int, uint64_t> freeSpaceByDisk() {
    std::map<int, uint64_t> result;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER freeBytes{};
        if (!GetDiskFreeSpaceExW(root, &freeBytes, nullptr, nullptr))
            continue;

        wchar_t vol[] = {L'\\', L'\\', L'.', L'\\', wchar_t(L'A' + i), L':', 0};
        HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        BYTE buf[512] = {};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf),
                            &got, nullptr)) {
            auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
            if (ext->NumberOfDiskExtents > 0)
                result[int(ext->Extents[0].DiskNumber)] += freeBytes.QuadPart;
        }
        CloseHandle(h);
    }
    return result;
}

} // namespace

WinStorageSource::~WinStorageSource() {
    for (auto& d : disks_)
        if (d.handle)
            CloseHandle(static_cast<HANDLE>(d.handle));
}

std::vector<DeviceInfo> WinStorageSource::discover() {
    std::vector<DeviceInfo> result;
    int ordinal = 0;
    for (int n = 0; n < 32; ++n) {
        wchar_t path[64];
        wsprintfW(path, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

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
    return result;
}

void WinStorageSource::sample(std::vector<Reading>& out) {
    std::map<int, uint64_t> freeMap = freeSpaceByDisk();
    for (auto& d : disks_) {
        HANDLE h = static_cast<HANDLE>(d.handle);
        if (d.sizeBytes > 0)
            out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Total", double(d.sizeBytes)});
        auto fit = freeMap.find(d.number);
        if (fit != freeMap.end()) {
            out.push_back(Reading{d.id, Quantity::Capacity, Unit::Byte, "Free", double(fit->second)});
            if (d.sizeBytes > 0) {
                uint64_t freeB = fit->second > d.sizeBytes ? d.sizeBytes : fit->second;
                out.push_back(Reading{d.id, Quantity::Level, Unit::Percent, "Used",
                                      100.0 * double(d.sizeBytes - freeB) / double(d.sizeBytes)});
            }
        }
        double tC = queryTemperature(h);
        if (!std::isnan(tC))
            out.push_back(Reading{d.id, Quantity::Temperature, Unit::Celsius, "Temperature", tC});
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
