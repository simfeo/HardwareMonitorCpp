// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// idimus_monitor — live console hardware monitor with a tabbed, htop-style TUI.
//
// The screen shows one hardware tab at a time with its metric drawn as a scrolling time graph;
// a tab bar sits at the bottom. Controls:
//   ←/→ or Tab   switch tab            1-9   jump to tab
//   Space        toggle the tab's mode  q     quit (Ctrl+C also works)
//
// Per-tab modes:
//   CPU        Overall load  <->  per logical-processor sparklines
//   GPU/…      Load          <->  Temperature
//
// Rendering: we use the terminal's alternate screen buffer and compose each frame into a single
// string, repainted in place from the home position with per-line erase — so there is no flicker.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "idimus_hw/idimus_hw.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h from defining min/max macros that clobber std::min/std::max
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace idimus_hw;

namespace
{

// ---- ANSI -----------------------------------------------------------------
constexpr const char* RESET = "\033[0m";
constexpr const char* BOLD = "\033[1m";
constexpr const char* INV = "\033[7m";
constexpr const char* GREEN = "\033[38;5;77m";
constexpr const char* YELLOW = "\033[38;5;179m";
constexpr const char* RED = "\033[38;5;167m";
constexpr const char* CYAN = "\033[38;5;80m";
constexpr const char* GREY = "\033[38;5;245m";
constexpr const char* HEAD = "\033[38;5;111m"; // section heads
constexpr const char* TRACK = "\033[38;5;238m";

// Eighth-block glyphs, indexed 0..8, for the top partial cell of a vertical bar.
const char* const BLOCKS[9] = {" ", "▁", "▂", "▃", "▄",
                               "▅", "▆", "▇", "█"};

// ---- terminal I/O ---------------------------------------------------------
#ifndef _WIN32
termios g_origTermios;
bool g_rawOn = false;
#endif

void enableRaw()
{
#ifndef _WIN32
    if (tcgetattr(STDIN_FILENO, &g_origTermios) == 0)
    {
        termios raw = g_origTermios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_rawOn = true;
    }
#endif
}

void disableRaw()
{
#ifndef _WIN32
    if (g_rawOn)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_origTermios);
        g_rawOn = false;
    }
#endif
}

void enterTui()
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
    {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    enableRaw();
    std::fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout); // alt buffer, hide cursor, clear once
    std::fflush(stdout);
}

void leaveTui()
{
    disableRaw();
    std::fputs("\033[?25h\033[?1049l", stdout); // show cursor, restore main buffer
    std::fflush(stdout);
}

void onSignal(int)
{
    leaveTui();
    std::_Exit(0);
}

void termSize(int& rows, int& cols)
{
    rows = 24;
    cols = 80;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    {
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
    if (rows < 12)
    {
        rows = 12;
    }
    if (cols < 40)
    {
        cols = 40;
    }
}

// ---- input ----------------------------------------------------------------
enum Key
{
    K_NONE = 0,
    K_QUIT,
    K_TOGGLE,
    K_NEXT,
    K_PREV,
    K_HELP,
    K_TAB1 = 10 // K_TAB1 + n selects tab n
};

// Maps a printable character to a Key code, or K_NONE. Shared by both input backends.
int keyForChar(int c)
{
    switch (c)
    {
        case 'q':
        case 'Q': return K_QUIT;
        case 'h':
        case 'H':
        case '?': return K_HELP;
        case 'n':
        case 'N':
        case '\t': return K_NEXT;
        case 'p':
        case 'P': return K_PREV;
        case 't':
        case 'T':
        case ' ': return K_TOGGLE;
        default: break;
    }
    if (c >= '1' && c <= '9')
    {
        return K_TAB1 + (c - '1');
    }
    return K_NONE;
}

// Waits up to timeoutMs for a key and maps it to a Key code (K_NONE on timeout).
int pollKey(int timeoutMs)
{
#ifdef _WIN32
    int waited = 0;
    for (;;)
    {
        if (_kbhit())
        {
            int c = _getch();
            if (c == 0 || c == 224) // arrow-key prefix
            {
                int c2 = _getch();
                if (c2 == 77)
                {
                    return K_NEXT; // right
                }
                if (c2 == 75)
                {
                    return K_PREV; // left
                }
                return K_NONE;
            }
            return keyForChar(c);
        }
        if (waited >= timeoutMs)
        {
            return K_NONE;
        }
        Sleep(15);
        waited += 15;
    }
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
    {
        return K_NONE;
    }
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1)
    {
        return K_NONE;
    }
    if (c == 27) // ESC — arrow keys arrive as ESC [ <A-D>
    {
        unsigned char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
            read(STDIN_FILENO, &seq[1], 1) == 1)
        {
            if (seq[1] == 'C')
            {
                return K_NEXT; // right
            }
            if (seq[1] == 'D' || seq[1] == 'Z')
            {
                return K_PREV; // left / shift-tab
            }
        }
        return K_NONE;
    }
    return keyForChar(c);
#endif
}

// ---- formatting helpers ---------------------------------------------------
const char* heatColor(double pct)
{
    return pct < 50 ? GREEN : pct < 80 ? YELLOW : RED;
}

std::string padLeft(const std::string& s, size_t w)
{
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

std::string truncPad(const std::string& s, size_t w)
{
    std::string out = s.size() > w ? s.substr(0, w) : s;
    out.append(w - out.size(), ' ');
    return out;
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

// A colored htop-style bar: [|||||||         42.0%] — used by the overview screen.
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

enum class Ax
{
    Pct,
    Temp,
    Rate
};

std::string axLabel(Ax ax, double v)
{
    if (ax == Ax::Rate)
    {
        return bytesRate(v);
    }
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f", v);
    return b;
}

// ---- history --------------------------------------------------------------
constexpr size_t MAX_HIST = 1024;
std::map<std::string, std::deque<double>> g_hist;

std::string histKey(const DeviceId& d, Quantity q, const std::string& ch)
{
    return toString(d) + "#" + std::to_string(static_cast<int>(q)) + "#" + ch;
}

// Appends every reading in the snapshot to its per-series ring buffer.
void record(const Snapshot& snap)
{
    for (const Reading& r : snap.readings())
    {
        auto& d = g_hist[histKey(r.device, r.quantity, r.channel)];
        d.push_back(r.value);
        if (d.size() > MAX_HIST)
        {
            d.pop_front();
        }
    }
}

const std::deque<double> g_empty;

const std::deque<double>& histFor(const Reading* r)
{
    if (!r)
    {
        return g_empty;
    }
    auto it = g_hist.find(histKey(r->device, r->quantity, r->channel));
    return it == g_hist.end() ? g_empty : it->second;
}

// ---- device view ----------------------------------------------------------
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

double valueOr(const Reading* r, double fallback)
{
    return r ? r->value : fallback;
}

std::string attr(const DeviceInfo& info, const std::string& key)
{
    auto it = info.attributes.find(key);
    return it == info.attributes.end() ? std::string() : it->second;
}

// CPU clock label varies by platform: Apple Silicon reports per-cluster ("P-Cluster"/"E-Cluster"),
// while Windows and Linux report "Core Clock". Try them in preference order so a clock shows on all.
const Reading* cpuClock(const DevView& d)
{
    const Reading* c = find(d, Quantity::Clock, "P-Cluster");
    if (!c)
    {
        c = find(d, Quantity::Clock, "Core"); // "Core Clock" on Windows/Linux
    }
    if (!c)
    {
        c = find(d, Quantity::Clock, "Cluster"); // E-Cluster fallback on macOS
    }
    return c;
}

// "1234 MHz" style trailing metric, or grey n/a — used by the overview screen.
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

// ---- tabs -----------------------------------------------------------------
enum class Cat
{
    Overview,
    Cpu,
    Gpu,
    Mem,
    Disk,
    Net,
    Bat
};

const char* catLabel(Cat c)
{
    switch (c)
    {
        case Cat::Overview: return "ALL";
        case Cat::Cpu: return "CPU";
        case Cat::Gpu: return "GPU";
        case Cat::Mem: return "MEM";
        case Cat::Disk: return "DISK";
        case Cat::Net: return "NET";
        case Cat::Bat: return "BAT";
    }
    return "?";
}

bool catOf(DeviceKind k, Cat& out)
{
    switch (k)
    {
        case DeviceKind::Cpu: out = Cat::Cpu; return true;
        case DeviceKind::GpuIntegrated:
        case DeviceKind::GpuDiscrete: out = Cat::Gpu; return true;
        case DeviceKind::Memory: out = Cat::Mem; return true;
        case DeviceKind::Storage: out = Cat::Disk; return true;
        case DeviceKind::Network: out = Cat::Net; return true;
        case DeviceKind::Battery: out = Cat::Bat; return true;
        default: return false;
    }
}

// UI state persists across frames.
struct Ui
{
    int tab = 0;
    bool cpuLogical = false;      // CPU tab: false=overall graph, true=per-core sparklines
    std::map<int, bool> showTemp; // other tabs, keyed by Cat: false=load, true=temperature
    bool help = false;            // help overlay shown
    std::string msg;              // transient status line (e.g. "temperature unavailable")
};
Ui g_ui;

// The categories currently shown as tabs, rebuilt each render; input handling reads this.
std::vector<Cat> g_tabCats;
// Per-tab: whether a temperature sensor is available (aligned with g_tabCats).
std::vector<bool> g_tabHasTemp;

void handleKey(int k)
{
    if (k == K_HELP)
    {
        g_ui.help = !g_ui.help;
        return;
    }
    g_ui.help = false; // any other action dismisses the help overlay
    g_ui.msg.clear();  // clear any prior status message on a new action

    int n = static_cast<int>(g_tabCats.size());
    if (n == 0)
    {
        return;
    }
    if (g_ui.tab >= n)
    {
        g_ui.tab = n - 1;
    }
    if (k == K_NEXT)
    {
        g_ui.tab = (g_ui.tab + 1) % n;
    }
    else if (k == K_PREV)
    {
        g_ui.tab = (g_ui.tab + n - 1) % n;
    }
    else if (k >= K_TAB1)
    {
        int idx = k - K_TAB1;
        if (idx < n)
        {
            g_ui.tab = idx;
        }
    }
    else if (k == K_TOGGLE)
    {
        Cat c = g_tabCats[g_ui.tab];
        if (c == Cat::Overview)
        {
            return; // overview has no mode to toggle
        }
        if (c == Cat::Cpu)
        {
            g_ui.cpuLogical = !g_ui.cpuLogical;
            return;
        }
        bool goingToTemp = !g_ui.showTemp[static_cast<int>(c)];
        bool hasTemp = g_ui.tab < static_cast<int>(g_tabHasTemp.size()) && g_tabHasTemp[g_ui.tab];
        if (goingToTemp && !hasTemp)
        {
            // Refuse to switch to a temperature view that has no sensor behind it.
            g_ui.msg = std::string("Temperature unavailable for ") + catLabel(c);
            return;
        }
        g_ui.showTemp[static_cast<int>(c)] = goingToTemp;
    }
}

// ---- graph rendering ------------------------------------------------------
// One column per historical sample, right-aligned (newest at the right). Each column is a
// vertical bar of eighth-blocks scaled into `height` rows and colored by its value.
std::vector<std::string> renderGraph(const std::deque<double>& h, double vmin, double vmax, int w,
                                     int height, Ax ax)
{
    if (w < 1)
    {
        w = 1;
    }
    if (height < 1)
    {
        height = 1;
    }
    if (vmax <= vmin)
    {
        vmax = vmin + 1;
    }

    std::vector<int> eighths(w, -1); // -1 = no sample for this column
    std::vector<double> pct(w, 0.0);
    for (int c = 0; c < w; ++c)
    {
        int fromRight = w - 1 - c;
        if (static_cast<size_t>(fromRight) < h.size())
        {
            double v = h[h.size() - 1 - fromRight];
            double frac = (v - vmin) / (vmax - vmin);
            frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
            eighths[c] = static_cast<int>(frac * height * 8 + 0.5);
            pct[c] = frac * 100.0;
        }
    }

    std::vector<std::string> rows;
    rows.reserve(height);
    for (int L = 0; L < height; ++L)
    {
        int cellFromBottom = height - 1 - L;
        std::string label;
        if (L == 0)
        {
            label = axLabel(ax, vmax);
        }
        else if (L == height - 1)
        {
            label = axLabel(ax, vmin);
        }
        else if (L == height / 2)
        {
            label = axLabel(ax, (vmin + vmax) / 2);
        }
        std::string row = std::string(GREY) + padLeft(label, 8) + " │" + RESET;
        for (int c = 0; c < w; ++c)
        {
            if (eighths[c] < 0)
            {
                row += ' ';
                continue;
            }
            int e = eighths[c] - cellFromBottom * 8;
            e = e < 0 ? 0 : e > 8 ? 8 : e;
            if (e == 0)
            {
                row += ' ';
            }
            else
            {
                row += heatColor(pct[c]);
                row += BLOCKS[e];
                row += RESET;
            }
        }
        rows.push_back(row);
    }
    return rows;
}

// A single-row graph (no axis) for compact per-item rows.
std::string sparkline(const std::deque<double>& h, double vmin, double vmax, int w)
{
    if (vmax <= vmin)
    {
        vmax = vmin + 1;
    }
    std::string s;
    for (int c = 0; c < w; ++c)
    {
        int fromRight = w - 1 - c;
        if (static_cast<size_t>(fromRight) >= h.size())
        {
            s += ' ';
            continue;
        }
        double v = h[h.size() - 1 - fromRight];
        double frac = (v - vmin) / (vmax - vmin);
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        int e = static_cast<int>(frac * 8 + 0.5);
        if (e <= 0)
        {
            s += ' ';
        }
        else
        {
            s += heatColor(frac * 100.0);
            s += BLOCKS[e];
            s += RESET;
        }
    }
    return s;
}

// The graphable series for a (category, device, temperature?) selection.
struct Series
{
    bool ok = false;
    std::deque<double> h;
    double cur = 0.0;
    double vmin = 0.0;
    double vmax = 100.0;
    Ax ax = Ax::Pct;
};

Series seriesFor(Cat cat, const DevView& d, bool temp)
{
    Series s;
    if (temp)
    {
        const Reading* r = find(d, Quantity::Temperature, "max");
        if (!r)
        {
            r = find(d, Quantity::Temperature, "");
        }
        if (r)
        {
            s.ok = true;
            s.h = histFor(r);
            s.cur = r->value;
            s.vmin = 0;
            s.vmax = 100;
            s.ax = Ax::Temp;
        }
        return s;
    }

    if (cat == Cat::Net) // "load" == combined throughput (auto-scaled)
    {
        const Reading* dn = find(d, Quantity::DataRate, "Download");
        const Reading* up = find(d, Quantity::DataRate, "Upload");
        if (dn || up)
        {
            const std::deque<double>& hd = histFor(dn);
            const std::deque<double>& hu = histFor(up);
            size_t n = std::max(hd.size(), hu.size());
            for (size_t k = 0; k < n; ++k) // k = 0 is newest
            {
                double a = k < hd.size() ? hd[hd.size() - 1 - k] : 0.0;
                double b = k < hu.size() ? hu[hu.size() - 1 - k] : 0.0;
                s.h.push_front(a + b);
            }
            double mx = 1.0e5; // floor at 100 KB/s so an idle link still scales sensibly
            for (double v : s.h)
            {
                mx = std::max(mx, v);
            }
            s.ok = true;
            s.cur = s.h.empty() ? 0.0 : s.h.back();
            s.vmin = 0;
            s.vmax = mx * 1.1;
            s.ax = Ax::Rate;
        }
        return s;
    }

    const Reading* r = nullptr;
    switch (cat)
    {
        case Cat::Cpu: r = find(d, Quantity::Load, "Total"); break;
        case Cat::Gpu: r = find(d, Quantity::Load, "Core"); break;
        case Cat::Mem: r = find(d, Quantity::Load, "Usage"); break;
        case Cat::Disk: r = find(d, Quantity::Load, "Activity"); break;
        case Cat::Bat: r = find(d, Quantity::Level, "Charge"); break;
        default: break;
    }
    if (r)
    {
        s.ok = true;
        s.h = histFor(r);
        s.cur = r->value;
        s.vmin = 0;
        s.vmax = 100;
        s.ax = Ax::Pct;
    }
    return s;
}

std::string curLabel(const Series& s)
{
    if (!s.ok)
    {
        return std::string(GREY) + "n/a" + RESET;
    }
    if (s.ax == Ax::Rate)
    {
        return bytesRate(s.cur);
    }
    char b[32];
    std::snprintf(b, sizeof(b), s.ax == Ax::Temp ? "%.0f°C" : "%.0f%%", s.cur);
    return b;
}

int coreIndex(const std::string& channel)
{
    size_t sp = channel.find_last_of(' ');
    return sp == std::string::npos ? 0 : std::atoi(channel.c_str() + sp + 1);
}

// ---- frame ----------------------------------------------------------------
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

void present(std::vector<std::string>& body, int rows)
{
    std::string f = "\033[H";
    for (int i = 0; i < rows; ++i)
    {
        if (i < static_cast<int>(body.size()))
        {
            f += body[i];
        }
        f += "\033[K";
        if (i + 1 < rows)
        {
            f += '\n';
        }
    }
    f += "\033[J";
    std::fwrite(f.data(), 1, f.size(), stdout);
    std::fflush(stdout);
}

// The original all-in-one dashboard: every subsystem at once. This is the start screen (tab 1).
void renderOverview(const std::map<DeviceId, DevView>& devs, std::vector<std::string>& body)
{
    auto push = [&](const std::string& s = "") { body.push_back(s); };
    auto head = [&](const char* label, const std::string& name)
    { push(std::string("  ") + HEAD + BOLD + label + RESET + "  " + name); };
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

    // CPU
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Cpu)
        {
            continue;
        }
        const Reading* load = find(d, Quantity::Load, "Total");
        const Reading* clk = cpuClock(d);
        const Reading* temp = find(d, Quantity::Temperature, "");
        const Reading* pwr = find(d, Quantity::Power, "Package");
        head("CPU ", d.info->name);
        push("    " + bar(valueOr(load, 0)) + "   " + GREY + "clock " + RESET +
             metric(clk, "%.0f MHz") + GREY + "  temp " + RESET + metric(temp, "%.0f°C") + GREY +
             "  power " + RESET + metric(pwr, "%.0f W"));
        push();
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
        push("    " + bar(valueOr(load, 0)) + "   " + GREY + "clock " + RESET +
             metric(clk, "%.0f MHz") + GREY + "  temp " + RESET + metric(temp, "%.0f°C") + GREY +
             "  power " + RESET + metric(pwr, "%.0f W"));
        push();
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
            tail = std::string(GREY) + "   " + RESET + bytesSize(used->value) + GREY + " / " + RESET +
                   bytesSize(total->value);
        }
        push("    " + bar(valueOr(use, 0)) + tail);
        push();
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
        push(std::string("  ") + HEAD + BOLD + "DISK" + RESET);
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
            std::string t = ssd ? std::string(GREY) + "  temp " + RESET + metric(temp, "%.0f°C")
                                : std::string(GREY) + "  (HDD)" + RESET;
            std::string freeTxt =
                freeB ? std::string(GREY) + "  free " + RESET + bytesSize(freeB->value)
                      : std::string();
            push("    " + truncPad(d.info->name, 22) + " " + GREY + "used " + RESET +
                 bar(valueOr(used, 0), 14) + freeTxt + t);
            push("    " + truncPad("", 22) + " " + GREY + "act  " + RESET +
                 bar(valueOr(act, 0), 14) + GREY + "  rd " + RESET + GREEN +
                 bytesRate(valueOr(rd, 0)) + RESET + GREY + "  wr " + RESET + YELLOW +
                 bytesRate(valueOr(wr, 0)) + RESET);
        }
        push();
    }

    // Network: only connected interfaces (link up), like Task Manager.
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
        push(std::string("  ") + HEAD + BOLD + "NET " + RESET);
        for (const DevView* d : active)
        {
            const Reading* dn = find(*d, Quantity::DataRate, "Download");
            const Reading* up = find(*d, Quantity::DataRate, "Upload");
            push("    " + truncPad(d->info->name, 22) + " " + GREY + "↓" + RESET + " " +
                 std::string(GREEN) + bytesRate(valueOr(dn, 0)) + RESET + "   " + GREY + "↑" + RESET +
                 " " + YELLOW + bytesRate(valueOr(up, 0)) + RESET);
        }
        push();
    }
}

void render(const Snapshot& snap)
{
    int rows = 0, cols = 0;
    termSize(rows, cols);
    int gw = std::max(20, std::min(cols - 12, 240)); // graph width
    int gh = std::max(5, std::min(rows - 11, 24));   // graph height

    if (g_ui.help)
    {
        struct Row
        {
            const char* keys;
            const char* what;
        };
        const Row rows_[] = {
            {"n / Tab", "next tab"},
            {"p / Shift+Tab", "previous tab"},
            {"Left / Right", "switch tab"},
            {"1 - 9", "jump to tab"},
            {"t / Space", "toggle mode (CPU: overall/logical, others: load/temp)"},
            {"h / ?", "toggle this help"},
            {"q", "quit"},
        };
        std::vector<std::string> body;
        body.push_back(std::string(BOLD) + CYAN + "  idimus_hw" + RESET + GREY + "  hotkeys" + RESET);
        body.push_back(std::string(TRACK) + "  " + std::string(std::min(cols - 4, 74), '-') + RESET);
        body.push_back("");
        for (const Row& r : rows_)
        {
            body.push_back(std::string("   ") + CYAN + truncPad(r.keys, 16) + RESET + GREY + r.what +
                           RESET);
        }
        body.push_back("");
        body.push_back(std::string("  ") + GREY + "press h to close" + RESET);
        present(body, rows);
        return;
    }

    // Group readings by device.
    std::map<DeviceId, DevView> devs;
    for (const DeviceInfo& d : snap.devices())
    {
        devs[d.id].info = &d;
    }
    for (const Reading& r : snap.readings())
    {
        devs[r.device].readings.push_back(&r);
    }

    // Build the ordered tab list from present hardware.
    struct Tab
    {
        Cat cat;
        std::vector<const DevView*> devs;
    };
    const Cat order[] = {Cat::Cpu, Cat::Gpu, Cat::Mem, Cat::Disk, Cat::Net, Cat::Bat};
    std::vector<Tab> tabs;
    // First tab is the overview dashboard (the start screen).
    {
        Tab ov;
        ov.cat = Cat::Overview;
        tabs.push_back(std::move(ov));
    }
    for (Cat cat : order)
    {
        Tab t;
        t.cat = cat;
        for (auto& kv : devs)
        {
            Cat c;
            if (!kv.second.info || !catOf(kv.second.info->id.kind, c) || c != cat)
            {
                continue;
            }
            // Network: only connected interfaces (link up), like Task Manager.
            if (cat == Cat::Net)
            {
                const Reading* link = find(kv.second, Quantity::Count, "Link");
                if (!link || link->value < 0.5)
                {
                    continue;
                }
            }
            t.devs.push_back(&kv.second);
        }
        if (!t.devs.empty())
        {
            tabs.push_back(std::move(t));
        }
    }

    g_tabCats.clear();
    g_tabHasTemp.clear();
    for (const Tab& t : tabs)
    {
        g_tabCats.push_back(t.cat);
        bool hasTemp = false;
        for (const DevView* d : t.devs)
        {
            if (find(*d, Quantity::Temperature, ""))
            {
                hasTemp = true;
                break;
            }
        }
        g_tabHasTemp.push_back(hasTemp);
    }
    if (devs.empty())
    {
        std::vector<std::string> empty{std::string("  ") + GREY + "no hardware sources" + RESET};
        present(empty, rows);
        return;
    }
    if (g_ui.tab >= static_cast<int>(tabs.size()))
    {
        g_ui.tab = static_cast<int>(tabs.size()) - 1;
    }
    const Tab& tab = tabs[g_ui.tab];

    std::vector<std::string> body;
    auto push = [&](const std::string& s = "") { body.push_back(s); };

    // Header.
    push(std::string(BOLD) + CYAN + "  idimus_hw" + RESET + GREY + "  hardware monitor" + RESET +
         "     " + GREY + nowClock() + RESET);
    push(std::string(TRACK) + "  " + std::string(std::min(cols - 4, 74), '-') + RESET);
    push();

    // Transient status message (e.g. temperature unavailable).
    if (!g_ui.msg.empty())
    {
        push(std::string("  ") + BOLD + YELLOW + "!  " + g_ui.msg + RESET);
        push();
    }

    if (tab.cat == Cat::Overview)
    {
        push(std::string("  ") + HEAD + BOLD + "OVERVIEW" + RESET + GREY + "  all hardware" + RESET);
        push();
        renderOverview(devs, body);
    }
    else
    {
        bool temp = g_ui.showTemp[static_cast<int>(tab.cat)];

        // Tab title line with mode indicator.
        std::string mode;
        if (tab.cat == Cat::Cpu)
        {
            mode = g_ui.cpuLogical ? "Logical processors" : "Overall";
        }
        else
        {
            mode = temp ? "Temperature" : "Load";
        }
        const DevView& first = *tab.devs.front();
        std::string devName = first.info ? first.info->name : catLabel(tab.cat);
        push(std::string("  ") + HEAD + BOLD + catLabel(tab.cat) + RESET + "  " + devName + "   " +
             GREY + "[ " + RESET + CYAN + mode + RESET + GREY + " ]" + RESET);
        push();

        if (tab.cat == Cat::Cpu && g_ui.cpuLogical)
        {
            // Per-logical-processor sparklines.
            std::vector<const Reading*> cores;
            for (const Reading* r : first.readings)
            {
                if (r->quantity == Quantity::Load && r->channel.rfind("Core", 0) == 0)
                {
                    cores.push_back(r);
                }
            }
            std::sort(cores.begin(), cores.end(), [](const Reading* a, const Reading* b)
                      { return coreIndex(a->channel) < coreIndex(b->channel); });
            int sw = std::max(10, std::min(cols - 22, 200));
            for (const Reading* r : cores)
            {
                char pctbuf[12];
                std::snprintf(pctbuf, sizeof(pctbuf), "%5.1f%%", r->value);
                push("  " + truncPad(r->channel, 8) + " " + sparkline(histFor(r), 0, 100, sw) + " " +
                     heatColor(r->value) + pctbuf + RESET);
            }
        }
        else if (tab.devs.size() == 1)
        {
            // Single device: full-height graph plus a stat line.
            Series s = seriesFor(tab.cat, first, temp);
            std::vector<std::string> g = renderGraph(s.h, s.vmin, s.vmax, gw, gh, s.ax);
            for (std::string& row : g)
            {
                push("  " + row);
            }
            std::string stat = std::string("  ") + std::string(9, ' ') + GREY + "now " + RESET +
                               curLabel(s);
            if (tab.cat == Cat::Cpu)
            {
                const Reading* clk = cpuClock(first);
                const Reading* tp = find(first, Quantity::Temperature, "");
                const Reading* pw = find(first, Quantity::Power, "Package");
                auto opt = [&](const char* name, const Reading* r, const char* fmt)
                {
                    if (!r)
                    {
                        return std::string();
                    }
                    char b[32];
                    std::snprintf(b, sizeof(b), fmt, r->value);
                    return std::string(GREY) + "  " + name + " " + RESET + b;
                };
                stat += opt("clock", clk, "%.0f MHz") + opt("temp", tp, "%.0f°C") +
                        opt("power", pw, "%.0f W");
            }
            push(stat);
        }
        else
        {
            // Multiple devices in one category: one sparkline row each.
            int sw = std::max(10, std::min(cols - 30, 200));
            for (const DevView* d : tab.devs)
            {
                Series s = seriesFor(tab.cat, *d, temp);
                std::string name = d->info ? d->info->name : "?";
                push("  " + truncPad(name, 18) + " " + sparkline(s.h, s.vmin, s.vmax, sw) + " " +
                     padLeft(curLabel(s), 9));
            }
        }
    }

    // Bottom tab bar + hint.
    std::string tabbar = "  ";
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        bool act = static_cast<int>(i) == g_ui.tab;
        std::string cell = std::string(" ") + std::to_string(i + 1) + ":" + catLabel(tabs[i].cat) +
                           " ";
        if (act)
        {
            tabbar += std::string(INV) + CYAN + cell + RESET;
        }
        else
        {
            tabbar += std::string(GREY) + cell + RESET;
        }
        tabbar += " ";
    }
    std::string hint = std::string("  ") + GREY +
                       "n/p tab   t toggle   1-9 jump   h help   q quit" + RESET;

    while (static_cast<int>(body.size()) < rows - 2)
    {
        push();
    }
    body.push_back(tabbar);
    body.push_back(hint);

    present(body, rows);
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

    using clock = std::chrono::steady_clock;
    auto period = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(interval));

    Snapshot last = monitor.poll();
    record(last);
    render(last);
    auto next = clock::now() + period;

    for (;;)
    {
        auto now = clock::now();
        if (now >= next)
        {
            last = monitor.poll();
            record(last);
            render(last);
            next = now + period;
            continue;
        }
        int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());
        if (ms > 250)
        {
            ms = 250; // stay responsive to keys/resize even with a long interval
        }
        int k = pollKey(ms);
        if (k == K_QUIT)
        {
            break;
        }
        if (k != K_NONE)
        {
            handleKey(k);
            render(last); // repaint immediately for a snappy tab/toggle switch
        }
    }
    return 0;
}
