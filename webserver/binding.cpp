// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// C ABI around idimus_hw for loading from Python via ctypes. Exposes create/destroy and a
// poll that returns the current snapshot as a JSON string.
#include <cstdio>
#include <string>

#include "idimus_hw/idimus_hw.hpp"

using namespace idimus_hw;

namespace
{

const char* quantityName(Quantity q)
{
    switch (q)
    {
        case Quantity::Temperature: return "temperature";
        case Quantity::Load: return "load";
        case Quantity::Power: return "power";
        case Quantity::Voltage: return "voltage";
        case Quantity::Current: return "current";
        case Quantity::Clock: return "clock";
        case Quantity::FanSpeed: return "fan";
        case Quantity::Energy: return "energy";
        case Quantity::Capacity: return "capacity";
        case Quantity::DataRate: return "datarate";
        case Quantity::DataVolume: return "datavolume";
        case Quantity::Level: return "level";
        case Quantity::Duration: return "duration";
        case Quantity::Count: return "count";
        case Quantity::Other: return "other";
    }
    return "other";
}

void appendEscaped(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
        }
    }
    out += '"';
}

void appendNumber(std::string& out, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    out += buf;
}

struct Handle
{
    Monitor monitor;
    std::string json;
};

} // namespace

extern "C"
{

#ifdef _WIN32
#define IHW_EXPORT __declspec(dllexport)
#else
#define IHW_EXPORT __attribute__((visibility("default")))
#endif

    IHW_EXPORT void* ihw_create()
    {
        auto* h = new Handle();
        h->monitor.addPlatformSources();
        h->monitor.open();
        h->monitor.poll(); // prime delta metrics
        return h;
    }

    IHW_EXPORT void ihw_destroy(void* p)
    {
        delete static_cast<Handle*>(p);
    }

    // Returns a JSON snapshot. The pointer is owned by the handle and valid until the next call.
    IHW_EXPORT const char* ihw_poll_json(void* p)
    {
        auto* h = static_cast<Handle*>(p);
        if (!h)
        {
            return "{}";
        }
        Snapshot snap = h->monitor.poll();

        std::string& o = h->json;
        o.clear();
        o += "{\"devices\":[";
        bool first = true;
        for (const DeviceInfo& d : snap.devices())
        {
            if (!first)
            {
                o += ',';
            }
            first = false;
            o += "{\"id\":";
            appendEscaped(o, toString(d.id));
            o += ",\"kind\":";
            appendEscaped(o, deviceKindName(d.id.kind));
            o += ",\"name\":";
            appendEscaped(o, d.name);
            o += ",\"attributes\":{";
            bool fa = true;
            for (const auto& kv : d.attributes)
            {
                if (!fa)
                {
                    o += ',';
                }
                fa = false;
                appendEscaped(o, kv.first);
                o += ':';
                appendEscaped(o, kv.second);
            }
            o += "}}";
        }
        o += "],\"readings\":[";
        first = true;
        for (const Reading& r : snap.readings())
        {
            if (!first)
            {
                o += ',';
            }
            first = false;
            o += "{\"device\":";
            appendEscaped(o, toString(r.device));
            o += ",\"quantity\":";
            appendEscaped(o, quantityName(r.quantity));
            o += ",\"unit\":";
            appendEscaped(o, unitSymbol(r.unit));
            o += ",\"channel\":";
            appendEscaped(o, r.channel);
            o += ",\"value\":";
            appendNumber(o, r.value);
            o += '}';
        }
        o += "]}";
        return o.c_str();
    }

} // extern "C"
