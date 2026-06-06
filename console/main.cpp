// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_monitor — live console hardware monitor with an htop-style full-screen TUI.
// Refreshes CPU/GPU/memory/storage/network every N seconds (default 1; pass the interval
// in seconds as the first argument).
//
// Rendering: we use the terminal's alternate screen buffer and compose each frame into a
// single string, then repaint it in place from the home position with per-line erase. The
// screen is never cleared between frames, so there is no flicker.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "idimus_hw/idimus_hw.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace idimus_hw;

namespace
{

// ---- ANSI -----------------------------------------------------------------
constexpr const char* RESET = "\033[0m";
constexpr const char* BOLD = "\033[1m";
constexpr const char* DIM = "\033[2m";
constexpr const char* GREEN = "\033[38;5;77m";
constexpr const char* YELLOW = "\033[38;5;179m";
constexpr const char* RED = "\033[38;5;167m";
constexpr const char* CYAN = "\033[38;5;80m";
constexpr const char* GREY = "\033[38;5;245m";
constexpr const char* HEAD = "\033[38;5;111m";  // section heads
constexpr const char* TRACK = "\033[38;5;238m"; // empty bar track

void enterTui()
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
    {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8); // for the ↓ ↑ glyphs
#endif
    std::fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout); // alt buffer, hide cursor, clear once
    std::fflush(stdout);
}

void leaveTui()
{
    std::fputs("\033[?25h\033[?1049l", stdout); // show cursor, restore main buffer
    std::fflush(stdout);
}

void onSignal(int)
{
    leaveTui();
    std::_Exit(0);
}

// ---- helpers --------------------------------------------------------------
struct DevView
{
    const DeviceInfo* info = nullptr;
    std::vector<const Reading*> readings;
};

const Reading* find(const DevView& d, Quantity q, const std::string& chanContains)
{
    for (const Reading* r : d.readings)
    {
        if (r->quantity == q && r->channel.find(chanContains) != std::string::npos)
        {
            return r;
        }
    }
    return nullptr;
}

std::string attr(const DeviceInfo& info, const std::string& key)
{
    auto it = info.attributes.find(key);
    return it == info.attributes.end() ? std::string() : it->second;
}

double valueOr(const Reading* r, double fallback)
{
    return r ? r->value : fallback;
}

const char* heatColor(double pct)
{
    return pct < 50 ? GREEN : pct < 80 ? YELLOW : RED;
}

std::string truncPad(const std::string& s, size_t w)
{
    std::string out = s.size() > w ? s.substr(0, w) : s;
    out.append(w - out.size(), ' ');
    return out;
}

// A colored htop-style bar: [|||||||         42.0%]
std::string bar(double pct, int width = 24)
{
    if (pct < 0)
    {
        pct = 0;
    }
    if (pct > 100)
    {
        pct = 100;
    }
    int fill = static_cast<int>(pct / 100.0 * width + 0.5);
    std::string s = GREY;
    s += "[";
    s += heatColor(pct);
    for (int i = 0; i < fill; ++i)
    {
        s += "|";
    }
    s += TRACK;
    for (int i = fill; i < width; ++i)
    {
        s += " ";
    }
    char pctbuf[16];
    std::snprintf(pctbuf, sizeof(pctbuf), "%5.1f%%", pct);
    s += GREY;
    s += "] ";
    s += RESET;
    s += pctbuf;
    return s;
}

// "1234 MHz" style trailing metric, or grey n/a.
std::string metric(const Reading* r, const char* fmt, double scale = 1.0)
{
    if (!r)
    {
        return std::string(GREY) + "n/a" + RESET;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, r->value * scale);
    return buf;
}

std::string humanBytes(double v, const char* const* u, int maxIdx)
{
    int i = 0;
    while (v >= 1024.0 && i < maxIdx)
    {
        v /= 1024.0;
        ++i;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}
std::string bytesRate(double v)
{
    static const char* u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    return humanBytes(v, u, 3);
}
std::string bytesSize(double v)
{
    static const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    return humanBytes(v, u, 4);
}

// Append one logical line: text, erase-to-end-of-line, newline.
void line(std::string& f, const std::string& text = "")
{
    f += text;
    f += "\033[K\n";
}

std::string nowClock()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

void render(const Snapshot& snap)
{
    std::map<DeviceId, DevView> devs;
    for (const DeviceInfo& d : snap.devices())
    {
        devs[d.id].info = &d;
    }
    for (const Reading& r : snap.readings())
    {
        devs[r.device].readings.push_back(&r);
    }

    std::string f;
    f.reserve(8192);
    f += "\033[H"; // home — no clear

    // header
    line(f, std::string(BOLD) + CYAN + "  idimus_hw " + RESET + GREY + " hardware monitor" + RESET +
                "      " + GREY + nowClock() + "   (Ctrl+C to quit)" + RESET);
    line(f, std::string(TRACK) + "  " + std::string(70, '-') + RESET);
    line(f);

    auto kindIs = [](DeviceKind k, std::initializer_list<DeviceKind> ks)
    {
        for (DeviceKind x : ks)
        {
            if (k == x)
            {
                return true;
            }
        }
        return false;
    };
    auto head = [&](const char* label, const std::string& name)
    { line(f, std::string("  ") + HEAD + BOLD + label + RESET + "  " + name); };

    // CPU
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Cpu)
        {
            continue;
        }
        const Reading* load = find(d, Quantity::Load, "Total");
        const Reading* clk = find(d, Quantity::Clock, "Core");
        const Reading* temp = find(d, Quantity::Temperature, "");
        const Reading* pwr = find(d, Quantity::Power, "Package");
        head("CPU ", d.info->name);
        line(f, "    " + bar(valueOr(load, 0)) + "   " + GREY + "clock " + RESET +
                    metric(clk, "%.0f MHz") + GREY + "  temp " + RESET + metric(temp, "%.0f°C") +
                    GREY + "  power " + RESET + metric(pwr, "%.0f W"));
        line(f);
    }

    // GPU
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info ||
            !kindIs(d.info->id.kind, {DeviceKind::GpuIntegrated, DeviceKind::GpuDiscrete}))
        {
            continue;
        }
        const Reading* load = find(d, Quantity::Load, "Core");
        const Reading* clk = find(d, Quantity::Clock, "Core");
        const Reading* temp = find(d, Quantity::Temperature, "");
        const Reading* pwr = find(d, Quantity::Power, "");
        head("GPU ", d.info->name);
        line(f, "    " + bar(valueOr(load, 0)) + "   " + GREY + "clock " + RESET +
                    metric(clk, "%.0f MHz") + GREY + "  temp " + RESET + metric(temp, "%.0f°C") +
                    GREY + "  power " + RESET + metric(pwr, "%.0f W"));
        line(f);
    }

    // Memory
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Memory)
        {
            continue;
        }
        const Reading* use = find(d, Quantity::Load, "Usage");
        const Reading* used = find(d, Quantity::DataVolume, "Used");
        const Reading* total = find(d, Quantity::DataVolume, "Total");
        head("MEM ", d.info->name);
        std::string tail;
        if (used && total)
        {
            tail = std::string(GREY) + "   " + RESET + bytesSize(used->value) + GREY + " / " +
                   RESET + bytesSize(total->value);
        }
        line(f, "    " + bar(valueOr(use, 0)) + tail);
        line(f);
    }

    // Storage
    bool anyDisk = false;
    for (auto& kv : devs)
    {
        if (kv.second.info && kv.second.info->id.kind == DeviceKind::Storage)
        {
            anyDisk = true;
        }
    }
    if (anyDisk)
    {
        line(f, std::string("  ") + HEAD + BOLD + "DISK" + RESET);
        for (auto& kv : devs)
        {
            const DevView& d = kv.second;
            if (!d.info || d.info->id.kind != DeviceKind::Storage)
            {
                continue;
            }
            const Reading* used = find(d, Quantity::Level, "Used");
            const Reading* temp = find(d, Quantity::Temperature, "");
            const Reading* freeB = find(d, Quantity::Capacity, "Free");
            const Reading* act = find(d, Quantity::Load, "Activity");
            const Reading* rd = find(d, Quantity::DataRate, "Read");
            const Reading* wr = find(d, Quantity::DataRate, "Write");
            bool ssd = attr(*d.info, "media") == "SSD";
            std::string t;
            if (ssd) // temperature shown for SSDs only
            {
                t = std::string(GREY) + "  temp " + RESET + metric(temp, "%.0f°C");
            }
            else
            {
                t = std::string(GREY) + "  (HDD)" + RESET;
            }
            std::string freeTxt =
                freeB ? std::string(GREY) + "  free " + RESET + bytesSize(freeB->value)
                      : std::string();
            line(f, "    " + truncPad(d.info->name, 22) + " " + GREY + "used " + RESET +
                        bar(valueOr(used, 0), 14) + freeTxt + t);
            // activity (busy time) + read/write throughput
            line(f, "    " + truncPad("", 22) + " " + GREY + "act  " + RESET +
                        bar(valueOr(act, 0), 14) + GREY + "  rd " + RESET + GREEN +
                        bytesRate(valueOr(rd, 0)) + RESET + GREY + "  wr " + RESET + YELLOW +
                        bytesRate(valueOr(wr, 0)) + RESET);
        }
        line(f);
    }

    // Network: only connected interfaces (link up), like Task Manager — stable regardless of
    // whether traffic is currently flowing.
    std::vector<const DevView*> active;
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Network)
        {
            continue;
        }
        const Reading* link = find(d, Quantity::Count, "Link");
        if (valueOr(link, 0) > 0.5)
        {
            active.push_back(&d);
        }
    }
    if (!active.empty())
    {
        line(f, std::string("  ") + HEAD + BOLD + "NET " + RESET);
        for (const DevView* d : active)
        {
            const Reading* dn = find(*d, Quantity::DataRate, "Download");
            const Reading* up = find(*d, Quantity::DataRate, "Upload");
            line(f, "    " + truncPad(d->info->name, 22) + " " + GREY + "↓" + RESET + " " +
                        std::string(GREEN) + bytesRate(valueOr(dn, 0)) + RESET + "   " + GREY +
                        "↑" + RESET + " " + YELLOW + bytesRate(valueOr(up, 0)) + RESET);
        }
        line(f);
    }

    f += "\033[J"; // clear anything left below from a previous, taller frame
    std::fwrite(f.data(), 1, f.size(), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    double interval = 1.0;
    if (argc > 1)
    {
        double v = std::atof(argv[1]);
        if (v >= 0.1)
        {
            interval = v;
        }
    }

    std::signal(SIGINT, onSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, onSignal);
#endif
    std::atexit(leaveTui);
    enterTui();

    Monitor monitor;
    monitor.addPlatformSources();
    monitor.open();
    monitor.poll(); // prime delta metrics

    auto period = std::chrono::duration<double>(interval);
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(period));
        render(monitor.poll());
    }
    return 0;
}
