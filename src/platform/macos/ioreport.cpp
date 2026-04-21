// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "platform/macos/ioreport.hpp"

#ifdef __APPLE__

#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <utility>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

namespace idimus_hw {
namespace mac {
namespace {

using FnCopyChannels = CFMutableDictionaryRef (*)(CFStringRef, CFStringRef, uint64_t, uint64_t,
                                                  uint64_t);
using FnMerge = void (*)(CFMutableDictionaryRef, CFMutableDictionaryRef, CFTypeRef);
using FnSubscribe = void* (*)(void*, CFMutableDictionaryRef, CFMutableDictionaryRef*, uint64_t,
                              CFTypeRef);
using FnSamples = CFDictionaryRef (*)(void*, CFMutableDictionaryRef, CFTypeRef);
using FnDelta = CFDictionaryRef (*)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
using FnIterate = void (*)(CFDictionaryRef, int (^)(CFDictionaryRef));
using FnStr = CFStringRef (*)(CFDictionaryRef);
using FnInt = int64_t (*)(CFDictionaryRef, int32_t);
using FnStateCount = int32_t (*)(CFDictionaryRef);
using FnStateResidency = int64_t (*)(CFDictionaryRef, int32_t);

FnCopyChannels copyChannels = nullptr;
FnMerge merge = nullptr;
FnSubscribe subscribe = nullptr;
FnSamples samples = nullptr;
FnDelta delta = nullptr;
FnIterate iterate = nullptr;
FnStr chanGroup = nullptr;
FnStr chanSubGroup = nullptr;
FnStr chanName = nullptr;
FnStr chanUnit = nullptr;
FnInt simpleValue = nullptr;
FnStateCount stateCount = nullptr;
FnStateResidency stateResidency = nullptr;

std::string toStd(CFStringRef s) {
    if (!s)
        return {};
    char b[160];
    return CFStringGetCString(s, b, sizeof(b), kCFStringEncodingUTF8) ? std::string(b)
                                                                      : std::string();
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double energyUnitToJoule(const std::string& u) {
    if (u == "mJ") return 1e-3;
    if (u == "uJ") return 1e-6;
    if (u == "nJ") return 1e-9;
    if (u == "J") return 1.0;
    return 1e-3;
}

// Apple CPU DVFS tables store an inverse-frequency code; MHz = K / code (empirically exact).
constexpr double kCpuCode = 65536000.0;

bool resolveSymbols(void* lib) {
    copyChannels = (FnCopyChannels)dlsym(lib, "IOReportCopyChannelsInGroup");
    merge = (FnMerge)dlsym(lib, "IOReportMergeChannels");
    subscribe = (FnSubscribe)dlsym(lib, "IOReportCreateSubscription");
    samples = (FnSamples)dlsym(lib, "IOReportCreateSamples");
    delta = (FnDelta)dlsym(lib, "IOReportCreateSamplesDelta");
    iterate = (FnIterate)dlsym(lib, "IOReportIterate");
    chanGroup = (FnStr)dlsym(lib, "IOReportChannelGetGroup");
    chanSubGroup = (FnStr)dlsym(lib, "IOReportChannelGetSubGroup");
    chanName = (FnStr)dlsym(lib, "IOReportChannelGetChannelName");
    chanUnit = (FnStr)dlsym(lib, "IOReportChannelGetUnitLabel");
    simpleValue = (FnInt)dlsym(lib, "IOReportSimpleGetIntegerValue");
    stateCount = (FnStateCount)dlsym(lib, "IOReportStateGetCount");
    stateResidency = (FnStateResidency)dlsym(lib, "IOReportStateGetResidency");
    return copyChannels && subscribe && samples && delta && iterate && chanGroup && chanName &&
           simpleValue && stateCount && stateResidency;
}

} // namespace

void IoReport::readDvfs() {
    io_registry_entry_t pmgr =
        IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/arm-io/pmgr");
    if (!pmgr)
        return;
    auto load = [&](const char* prop, std::vector<double>& out, bool gpu) {
        CFStringRef k = CFStringCreateWithCString(nullptr, prop, kCFStringEncodingUTF8);
        auto data = (CFDataRef)IORegistryEntryCreateCFProperty(pmgr, k, kCFAllocatorDefault, 0);
        CFRelease(k);
        if (!data)
            return;
        const uint8_t* b = CFDataGetBytePtr(data);
        long len = CFDataGetLength(data);
        for (long i = 0; i + 8 <= len; i += 8) {
            uint32_t code;
            std::memcpy(&code, b + i, 4);
            if (gpu)
                out.push_back(code / 1e6); // already Hz; keeps leading 0 = idle
            else if (code != 0)
                out.push_back(std::round(kCpuCode / code));
        }
        CFRelease(data);
    };
    load("voltage-states1", eCpuMhz_, false);
    load("voltage-states5", pCpuMhz_, false);
    load("voltage-states9", gpuMhz_, true);
    IOObjectRelease(pmgr);
}

IoReport::IoReport() {
    lib_ = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!lib_ || !resolveSymbols(lib_))
        return;

    readDvfs();

    CFMutableDictionaryRef chans = copyChannels(CFSTR("Energy Model"), nullptr, 0, 0, 0);
    if (!chans)
        return;
    if (merge) {
        if (auto c = copyChannels(CFSTR("CPU Stats"), nullptr, 0, 0, 0)) { merge(chans, c, nullptr); CFRelease(c); }
        if (auto g = copyChannels(CFSTR("GPU Stats"), nullptr, 0, 0, 0)) { merge(chans, g, nullptr); CFRelease(g); }
    }
    CFMutableDictionaryRef subbed = nullptr;
    void* sub = subscribe(nullptr, chans, &subbed, 0, nullptr);
    CFRelease(chans);
    if (!sub || !subbed)
        return;
    sub_ = sub;
    chans_ = subbed;
    ok_ = true;
}

IoReport::~IoReport() {
    if (prev_)
        CFRelease((CFDictionaryRef)prev_);
    if (chans_)
        CFRelease((CFMutableDictionaryRef)chans_);
    if (lib_)
        dlclose(lib_);
}

bool IoReport::sample(Sample& out) {
    out.powerW.clear();
    out.freqMhz.clear();
    if (!ok_)
        return false;

    double t = now();
    CFDictionaryRef cur = samples(sub_, (CFMutableDictionaryRef)chans_, nullptr);
    if (!cur)
        return false;
    if (!prev_) {
        prev_ = (void*)cur;
        prevT_ = t;
        return false;
    }
    double dt = t - prevT_;
    if (dt <= 0)
        dt = 1.0;

    CFDictionaryRef d = delta((CFDictionaryRef)prev_, cur, nullptr);
    __block std::map<std::string, double> joule;
    __block std::map<std::string, std::pair<double, double>> freq; // {sum(res*MHz), sum(res)}
    IoReport* self = this;

    if (d) {
        iterate(d, ^int(CFDictionaryRef ch) {
            std::string group = toStd(chanGroup(ch));
            std::string name = toStd(chanName(ch));

            if (group == "Energy Model") {
                std::string unit = chanUnit ? toStd(chanUnit(ch)) : "mJ";
                double j = double(simpleValue(ch, 0)) * energyUnitToJoule(unit);
                if (name.find("GPU") != std::string::npos) joule["GPU"] += j;
                else if (name.find("ANE") != std::string::npos) joule["ANE"] += j;
                else if (name.find("CPU") != std::string::npos) joule["CPU"] += j;
                return 0;
            }

            std::string subg = chanSubGroup ? toStd(chanSubGroup(ch)) : "";
            if (subg == "CPU Complex Performance States") {
                const std::vector<double>* tbl = nullptr;
                std::string dom;
                if (name == "ECPU") { tbl = &self->eCpuMhz_; dom = "E-CPU"; }
                else if (name == "PCPU" || name == "PCPU1") { tbl = &self->pCpuMhz_; dom = "P-CPU"; }
                if (tbl) {
                    int n = stateCount(ch);
                    auto& acc = freq[dom];
                    for (int i = 1; i < n; ++i) {
                        size_t idx = size_t(i - 1);
                        if (idx >= tbl->size()) break;
                        double r = double(stateResidency(ch, i));
                        acc.first += r * (*tbl)[idx];
                        acc.second += r;
                    }
                }
                return 0;
            }
            if (group == "GPU Stats" && subg.find("Performance States") != std::string::npos &&
                !self->gpuMhz_.empty()) {
                int n = stateCount(ch);
                auto& acc = freq["GPU"];
                for (int i = 0; i < n; ++i) {
                    size_t idx = size_t(i);
                    if (idx >= self->gpuMhz_.size()) break;
                    double mhz = self->gpuMhz_[idx];
                    if (mhz <= 0) continue;
                    double r = double(stateResidency(ch, i));
                    acc.first += r * mhz;
                    acc.second += r;
                }
            }
            return 0;
        });
        CFRelease(d);
    }

    for (auto& kv : joule)
        out.powerW[kv.first] = kv.second / dt;
    for (auto& kv : freq)
        if (kv.second.second > 0)
            out.freqMhz[kv.first] = kv.second.first / kv.second.second;

    CFRelease((CFDictionaryRef)prev_);
    prev_ = (void*)cur;
    prevT_ = t;
    return !out.powerW.empty() || !out.freqMhz.empty();
}

} // namespace mac
} // namespace idimus_hw

#endif // __APPLE__
