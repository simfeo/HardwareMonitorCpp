// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/win_gpu_source.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "platform/windows/win_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {

constexpr unsigned kVendorIntel = 0x8086;
constexpr uint64_t kIntegratedDedicatedMax = 512ull * 1024 * 1024;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(::tolower(c)); });
    return s;
}

const char* vendorName(unsigned id) {
    switch (id) {
        case 0x8086: return "Intel";
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        default: return "";
    }
}

std::vector<std::pair<std::string, double>> readArray(PDH_HCOUNTER counter) {
    std::vector<std::pair<std::string, double>> result;
    DWORD bufSize = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &count, nullptr) !=
            PDH_MORE_DATA ||
        bufSize == 0)
        return result;
    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &count, items) !=
        ERROR_SUCCESS)
        return result;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
            result.emplace_back(lower(win::wideToUtf8(items[i].szName)),
                                items[i].FmtValue.doubleValue);
    return result;
}

} // namespace

WinGpuSource::~WinGpuSource() {
    if (query_)
        PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
}

std::vector<DeviceInfo> WinGpuSource::discover() {
    std::vector<DeviceInfo> result;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        !factory)
        return result;

    int ordinal = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        // Intel integrated only: discrete AMD/Intel/NVIDIA GPUs are covered by their telemetry
        // SDKs (ADL/IGCL/NVML). PDH is the reliable, SDK-free path for the iGPU.
        DeviceKind kind = DeviceKind::GpuIntegrated;
        bool keep = SUCCEEDED(adapter->GetDesc1(&desc)) &&
                    !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == kVendorIntel &&
                    desc.DedicatedVideoMemory <= kIntegratedDedicatedMax;
        if (keep) {
            Adapter a;
            a.id = DeviceId{kind, ordinal};
            char tag[48];
            std::snprintf(tag, sizeof(tag), "luid_0x%08x_0x%08x",
                          unsigned(desc.AdapterLuid.HighPart), unsigned(desc.AdapterLuid.LowPart));
            a.luidTag = lower(tag);
            adapters_.push_back(a);

            DeviceInfo info;
            info.id = a.id;
            info.name = win::wideToUtf8(desc.Description);
            info.vendor = vendorName(desc.VendorId);
            if (desc.DedicatedVideoMemory > 0)
                info.attributes["dedicated_vram_bytes"] = std::to_string(desc.DedicatedVideoMemory);
            result.push_back(std::move(info));
            ++ordinal;
        }
        adapter->Release();
    }
    factory->Release();

    if (!adapters_.empty()) {
        PDH_HQUERY q = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
            query_ = q;
            PDH_HCOUNTER c;
            if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) ==
                ERROR_SUCCESS)
                cUtil_ = c;
            if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c) ==
                ERROR_SUCCESS)
                cDed_ = c;
            if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &c) ==
                ERROR_SUCCESS)
                cShared_ = c;
            PdhCollectQueryData(q); // prime utilization deltas
        }
    }
    return result;
}

void WinGpuSource::sample(std::vector<Reading>& out) {
    if (!query_ || adapters_.empty())
        return;
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) != ERROR_SUCCESS)
        return;

    auto util = cUtil_ ? readArray(static_cast<PDH_HCOUNTER>(cUtil_))
                       : std::vector<std::pair<std::string, double>>{};
    auto ded = cDed_ ? readArray(static_cast<PDH_HCOUNTER>(cDed_))
                     : std::vector<std::pair<std::string, double>>{};
    auto shared = cShared_ ? readArray(static_cast<PDH_HCOUNTER>(cShared_))
                           : std::vector<std::pair<std::string, double>>{};

    for (const Adapter& a : adapters_) {
        std::map<std::string, double> byEngtype; // busiest engine type = GPU load
        for (const auto& kv : util) {
            if (kv.first.find(a.luidTag) == std::string::npos)
                continue;
            std::string eng = "?";
            size_t p = kv.first.find("engtype_");
            if (p != std::string::npos)
                eng = kv.first.substr(p + 8);
            byEngtype[eng] += kv.second;
        }
        double load = 0;
        for (const auto& kv : byEngtype)
            load = std::max(load, kv.second);
        out.push_back(Reading{a.id, Quantity::Load, Unit::Percent, "Core", std::min(load, 100.0)});

        double dedBytes = 0, sharedBytes = 0;
        for (const auto& kv : ded)
            if (kv.first.find(a.luidTag) != std::string::npos)
                dedBytes += kv.second;
        for (const auto& kv : shared)
            if (kv.first.find(a.luidTag) != std::string::npos)
                sharedBytes += kv.second;
        out.push_back(Reading{a.id, Quantity::DataVolume, Unit::Byte, "Memory Dedicated", dedBytes});
        out.push_back(Reading{a.id, Quantity::DataVolume, Unit::Byte, "Memory Shared", sharedBytes});
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
