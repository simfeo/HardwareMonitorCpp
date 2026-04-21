// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "platform/macos/smc.hpp"

#ifdef __APPLE__

#include <cmath>
#include <cstring>

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

namespace idimus_hw {
namespace mac {
namespace {

// SMC AppleSMC kernel method + command selectors.
constexpr uint32_t kMethod = 2;
constexpr uint8_t kCmdReadBytes = 5;
constexpr uint8_t kCmdReadIndex = 8;
constexpr uint8_t kCmdReadKeyInfo = 9;

// SMC parameter block. NATURAL alignment is required — the kernel rejects a packed layout.
struct KeyInfo {
    uint32_t dataSize;
    uint32_t dataType;
    uint8_t attributes;
};
struct Version {
    uint8_t major, minor, build, reserved;
    uint16_t release;
};
struct PLimit {
    uint16_t version, length;
    uint32_t cpu, gpu, mem;
};
struct Param {
    uint32_t key;
    Version version;
    PLimit plimit;
    KeyInfo keyInfo;
    uint8_t result;
    uint8_t status;
    uint8_t cmd;
    uint32_t index;
    uint8_t bytes[32];
};

uint32_t fourcc(const std::string& s) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | (i < (int)s.size() ? (uint8_t)s[i] : 0);
    return v;
}

void fourccToStr(uint32_t v, char out[5]) {
    out[0] = char(v >> 24);
    out[1] = char(v >> 16);
    out[2] = char(v >> 8);
    out[3] = char(v);
    out[4] = 0;
}

float decode(const char type[5], const uint8_t* b, uint32_t size) {
    std::string t(type);
    if (t == "flt " && size == 4) {
        float f;
        std::memcpy(&f, b, 4);
        return f;
    }
    if (t == "sp78" && size == 2)
        return int16_t((b[0] << 8) | b[1]) / 256.0f;
    if (t == "ui8 " && size >= 1)
        return float(b[0]);
    if (t == "ui16" && size >= 2)
        return float((b[0] << 8) | b[1]);
    if (t == "ui32" && size >= 4)
        return float((uint32_t(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    // fpXY / spXY fixed point: Y fractional bits encoded in the last type nibble.
    if ((t[0] == 'f' || t[0] == 's') && t[1] == 'p' && size == 2) {
        int frac = 0;
        char c = t[3];
        if (c >= '0' && c <= '9') frac = c - '0';
        else if (c >= 'a' && c <= 'f') frac = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') frac = c - 'A' + 10;
        uint16_t raw = uint16_t((b[0] << 8) | b[1]);
        float scale = float(1 << frac);
        return (t[0] == 's') ? int16_t(raw) / scale : raw / scale;
    }
    return std::nanf("");
}

} // namespace

Smc::Smc() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc)
        return;
    io_connect_t c = 0;
    if (IOServiceOpen(svc, mach_task_self(), 0, &c) == kIOReturnSuccess)
        conn_ = c;
    IOObjectRelease(svc);
}

Smc::~Smc() {
    if (conn_)
        IOServiceClose(conn_);
}

bool Smc::readFloat(const std::string& key, float& out) {
    if (!conn_)
        return false;
    Param in{}, res{};
    in.key = fourcc(key);
    in.cmd = kCmdReadKeyInfo;
    size_t sz = sizeof(Param);
    if (IOConnectCallStructMethod(conn_, kMethod, &in, sz, &res, &sz) != kIOReturnSuccess)
        return false;
    uint32_t dataSize = res.keyInfo.dataSize;
    if (dataSize == 0 || dataSize > sizeof(res.bytes))
        return false;

    in.keyInfo.dataSize = dataSize;
    in.cmd = kCmdReadBytes;
    Param val{};
    sz = sizeof(Param);
    if (IOConnectCallStructMethod(conn_, kMethod, &in, sizeof(Param), &val, &sz) != kIOReturnSuccess)
        return false;

    char type[5];
    fourccToStr(res.keyInfo.dataType, type);
    float f = decode(type, val.bytes, dataSize);
    if (std::isnan(f))
        return false;
    out = f;
    return true;
}

std::vector<std::string> Smc::allKeys() {
    std::vector<std::string> keys;
    if (!conn_)
        return keys;

    // "#KEY" (ui32) holds the controller's key count.
    float countF = 0;
    if (!readFloat("#KEY", countF) || countF <= 0)
        return keys;
    uint32_t count = uint32_t(countF);

    for (uint32_t i = 0; i < count; ++i) {
        Param in{}, res{};
        in.cmd = kCmdReadIndex;
        in.index = i;
        size_t sz = sizeof(Param);
        if (IOConnectCallStructMethod(conn_, kMethod, &in, sz, &res, &sz) != kIOReturnSuccess)
            continue;
        char name[5];
        fourccToStr(res.key, name);
        if (name[0])
            keys.emplace_back(name);
    }
    return keys;
}

} // namespace mac
} // namespace idimus_hw

#endif // __APPLE__
