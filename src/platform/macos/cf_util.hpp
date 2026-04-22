// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Small inline CoreFoundation / IORegistry helpers shared by the macOS IOKit sources.
#pragma once

#ifdef __APPLE__

#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

namespace idimus_hw {
namespace mac {

inline std::string cfStr(CFStringRef s) {
    if (!s)
        return {};
    char buf[256];
    return CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) ? std::string(buf)
                                                                          : std::string();
}

// Reads an integer IORegistry property; returns false if absent / not a number.
inline bool regNumber(io_registry_entry_t e, CFStringRef key, long long& out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    bool ok = v && CFGetTypeID(v) == CFNumberGetTypeID() &&
              CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &out);
    if (v)
        CFRelease(v);
    return ok;
}

inline bool regBool(io_registry_entry_t e, CFStringRef key, bool& out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    bool ok = v && CFGetTypeID(v) == CFBooleanGetTypeID();
    if (ok)
        out = CFBooleanGetValue((CFBooleanRef)v);
    if (v)
        CFRelease(v);
    return ok;
}

inline std::string regString(io_registry_entry_t e, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    std::string out;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        out = cfStr((CFStringRef)v);
    if (v)
        CFRelease(v);
    return out;
}

// Reads a number from a dictionary property (optionally an ancestor's), e.g. AGX
// "PerformanceStatistics" or storage "Device Characteristics".
inline bool dictNumber(CFDictionaryRef d, const char* key, double& out) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    CFRelease(k);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID())
        return false;
    return CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &out);
}

inline std::string searchDictString(io_registry_entry_t e, const char* dictKey,
                                    const char* itemKey) {
    CFStringRef dk = CFStringCreateWithCString(nullptr, dictKey, kCFStringEncodingUTF8);
    CFTypeRef prop = IORegistryEntrySearchCFProperty(
        e, kIOServicePlane, dk, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    CFRelease(dk);
    std::string out;
    if (prop && CFGetTypeID(prop) == CFDictionaryGetTypeID()) {
        CFStringRef ik = CFStringCreateWithCString(nullptr, itemKey, kCFStringEncodingUTF8);
        CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)prop, ik);
        CFRelease(ik);
        if (v && CFGetTypeID(v) == CFStringGetTypeID())
            out = cfStr((CFStringRef)v);
    }
    if (prop)
        CFRelease(prop);
    return out;
}

} // namespace mac
} // namespace idimus_hw

#endif // __APPLE__
