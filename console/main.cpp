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
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
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

#ifdef __APPLE__
#include <mach-o/dyld.h>
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
constexpr const char* BLUE = "\033[38;5;39m"; // swap overlay on the memory graph
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
    K_UP,
    K_DOWN,
    K_ZOOM_OUT, // '-' : longer span, more time per graph column
    K_ZOOM_IN,  // '=' : shorter span, back toward live
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
        case 'k': return K_UP;
        case 'j': return K_DOWN;
        case 't':
        case 'T':
        case '\r':
        case '\n':
        case ' ': return K_TOGGLE;
        case '-':
        case '_': return K_ZOOM_OUT;
        case '=':
        case '+': return K_ZOOM_IN;
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
                if (c2 == 72)
                {
                    return K_UP;
                }
                if (c2 == 80)
                {
                    return K_DOWN;
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
            if (seq[1] == 'A')
            {
                return K_UP;
            }
            if (seq[1] == 'B')
            {
                return K_DOWN;
            }
        }
        return K_NONE;
    }
    return keyForChar(c);
#endif
}

// ---- settings (persisted next to the binary as YAML) ----------------------
// User choices of which network interfaces / storage devices to show. Only devices the user has
// explicitly toggled appear here; anything else falls back to the default heuristic.
struct Settings
{
    std::map<std::string, bool> net;      // key: interface name (e.g. "en0")
    std::map<std::string, bool> storage;  // key: stable device id (BSD name where available)
    std::string cpuCoreStyle = "rows";    // "rows" (one sparkline each) or "grid" (Task-Manager)
    std::string swapScale = "fullness";   // "fullness" (% of swap total) or "ram" (% of RAM total)
    int zoom = 0;                         // index into g_zoomLevels (seconds per graph column)
};
Settings g_settings;
std::string g_configPath;

// Zoom levels expressed as seconds of real time per graph column. Index 0 is "live" (one sample per
// column). '-' steps toward the end (coarser/longer), '=' steps back toward live.
const int g_zoomLevels[] = {1, 5, 15, 30, 60, 300};
constexpr int g_zoomCount = static_cast<int>(sizeof(g_zoomLevels) / sizeof(g_zoomLevels[0]));

int zoomSecPerCol()
{
    int z = g_settings.zoom;
    if (z < 0)
    {
        z = 0;
    }
    if (z >= g_zoomCount)
    {
        z = g_zoomCount - 1;
    }
    return g_zoomLevels[z];
}

// Human label for the current zoom, e.g. "live", "15s/col", "5m/col".
std::string zoomLabel()
{
    if (g_settings.zoom <= 0)
    {
        return "live";
    }
    int s = zoomSecPerCol();
    char b[24];
    if (s >= 60)
    {
        std::snprintf(b, sizeof(b), "%dm/col", s / 60);
    }
    else
    {
        std::snprintf(b, sizeof(b), "%ds/col", s);
    }
    return b;
}

// Directory holding the running executable, so the config can live right next to it.
std::string exeDir()
{
    std::string path;
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
    if (n > 0)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, n, nullptr, 0, nullptr, nullptr);
        path.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, wbuf, n, &path[0], len, nullptr, nullptr);
    }
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0)
    {
        char real[PATH_MAX];
        path = realpath(buf, real) ? real : buf;
    }
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        path = buf;
    }
#endif
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string trimStr(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Minimal reader for our tiny two-section YAML: `network:` / `storage:` each with `  key: bool`.
void loadSettings(const std::string& path)
{
    std::ifstream f(path);
    if (!f)
    {
        return;
    }
    std::string line, section;
    while (std::getline(f, line))
    {
        size_t hash = line.find('#'); // strip comments
        if (hash != std::string::npos)
        {
            line = line.substr(0, hash);
        }
        if (trimStr(line).empty())
        {
            continue;
        }
        bool indented = line[0] == ' ' || line[0] == '\t';
        std::string t = trimStr(line);
        size_t colon = t.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        std::string key = trimStr(t.substr(0, colon));
        std::string val = trimStr(t.substr(colon + 1));
        if (!indented)
        {
            section = key;
            continue;
        }
        if (section == "options")
        {
            if (key == "cpu_cores" && (val == "rows" || val == "grid"))
            {
                g_settings.cpuCoreStyle = val;
            }
            else if (key == "swap_scale" && (val == "fullness" || val == "ram"))
            {
                g_settings.swapScale = val;
            }
            else if (key == "zoom")
            {
                g_settings.zoom = std::atoi(val.c_str());
            }
            continue;
        }
        bool b = val == "true" || val == "1" || val == "yes" || val == "on";
        if (section == "network")
        {
            g_settings.net[key] = b;
        }
        else if (section == "storage")
        {
            g_settings.storage[key] = b;
        }
    }
}

void saveSettings()
{
    if (g_configPath.empty())
    {
        return;
    }
    std::ofstream f(g_configPath, std::ios::trunc);
    if (!f)
    {
        return;
    }
    f << "# idimus_monitor settings — also editable from the Settings tab.\n";
    f << "# true = show the device, false = hide it.\n\n";
    f << "options:\n";
    f << "  cpu_cores: " << g_settings.cpuCoreStyle << "\n";
    f << "  swap_scale: " << g_settings.swapScale << "\n";
    f << "  zoom: " << g_settings.zoom << "\n";
    f << "\nnetwork:\n";
    for (const auto& kv : g_settings.net)
    {
        f << "  " << kv.first << ": " << (kv.second ? "true" : "false") << "\n";
    }
    f << "\nstorage:\n";
    for (const auto& kv : g_settings.storage)
    {
        f << "  " << kv.first << ": " << (kv.second ? "true" : "false") << "\n";
    }
}

// Default-hidden pseudo-interfaces on macOS (awdl/llw/bridge/ap/anpi/utun/…), mirroring how the
// Windows source already drops virtual adapters. Users can still force any of these on in Settings.
bool isVirtualIface(const std::string& n)
{
    static const char* const pfx[] = {"awdl", "llw",   "bridge", "ap",     "anpi", "utun", "gif",
                                      "stf",  "vmnet", "XHC",    "pdp_ip", "p2p",  "lo"};
    for (const char* p : pfx)
    {
        if (n.rfind(p, 0) == 0)
        {
            return true;
        }
    }
    return false;
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
    Rate,
    Bytes
};

std::string axLabel(Ax ax, double v)
{
    if (ax == Ax::Rate)
    {
        return bytesRate(v);
    }
    if (ax == Ax::Bytes)
    {
        return bytesSize(v);
    }
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f", v);
    return b;
}

// ---- history --------------------------------------------------------------
constexpr size_t MAX_HIST = 8192; // ~2.2 h at the default 1 s interval; covers the coarse zooms
std::map<std::string, std::deque<double>> g_hist;
double g_interval = 1.0; // seconds between polls; set in main, used to convert zoom to samples

enum class Agg
{
    Avg,
    Peak,
    Min
};

// How many raw samples fall in one graph column at the current zoom (1 == live, no bucketing).
int columnSamples()
{
    if (g_settings.zoom <= 0)
    {
        return 1;
    }
    return std::max(1, static_cast<int>(std::lround(zoomSecPerCol() / g_interval)));
}

// Downsample history into `w` columns, aggregating each `per`-sample bucket. Newest sample stays on
// the right. `per <= 1` returns the raw history, so live zoom behaves exactly as before.
std::deque<double> bucketHistory(const std::deque<double>& h, int w, int per, Agg agg)
{
    if (per <= 1 || w <= 0 || h.empty())
    {
        return h;
    }
    std::deque<double> out;
    int n = static_cast<int>(h.size());
    for (int end = n; end > 0 && static_cast<int>(out.size()) < w; end -= per)
    {
        int start = std::max(0, end - per);
        double acc = h[start];
        for (int i = start + 1; i < end; ++i)
        {
            double v = h[i];
            if (agg == Agg::Peak)
            {
                acc = std::max(acc, v);
            }
            else if (agg == Agg::Min)
            {
                acc = std::min(acc, v);
            }
            else
            {
                acc += v;
            }
        }
        if (agg == Agg::Avg)
        {
            acc /= (end - start);
        }
        out.push_front(acc);
    }
    return out;
}

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

bool netLinkUp(const DevView& d)
{
    const Reading* l = find(d, Quantity::Count, "Link");
    return l && l->value > 0.5;
}

// Whether a network interface should appear. Explicit user choice wins; otherwise the default is
// "connected and not a known virtual adapter".
bool showNet(const DevView& d)
{
    std::string name = d.info ? d.info->name : std::string();
    auto it = g_settings.net.find(name);
    if (it != g_settings.net.end())
    {
        return it->second;
    }
    if (isVirtualIface(name))
    {
        return false;
    }
    return netLinkUp(d);
}

// Stable settings key for a storage device: prefer the BSD name (unique) over the display name,
// which can repeat across synthesized volumes.
std::string storageKey(const DevView& d)
{
    if (!d.info)
    {
        return std::string();
    }
    std::string bsd = attr(*d.info, "bsd_name");
    return bsd.empty() ? d.info->name : bsd;
}

bool showStorage(const DevView& d)
{
    auto it = g_settings.storage.find(storageKey(d));
    return it == g_settings.storage.end() ? true : it->second;
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
    Bat,
    Settings
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
        case Cat::Settings: return "SET";
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
    std::map<int, int> view; // selected view within a tab, keyed by Cat (0 = primary load view)
    bool help = false;       // help overlay shown
    std::string msg;         // transient status line (e.g. "no thermal sensors")
    int setSel = 0;          // selected row in the Settings tab
};
Ui g_ui;

// The categories currently shown as tabs, rebuilt each render; input handling reads this.
std::vector<Cat> g_tabCats;
// Per-tab: number of cyclable views (load + thermal graphs), aligned with g_tabCats.
std::vector<int> g_tabViewCount;

// One selectable row in the Settings tab, rebuilt each render; input handling reads this.
struct SetItem
{
    enum Kind
    {
        Net,     // network interface (toggle visibility)
        Storage, // storage device (toggle visibility)
        Option,  // a two-state display option (toggle its value)
        Choice   // a multi-value option (cycles through a list on toggle)
    } kind;
    std::string key; // settings key to toggle
    bool on;         // current state (shown, or option enabled)
};
std::vector<SetItem> g_setItems;

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

    // Tab navigation works from any tab.
    if (k == K_NEXT)
    {
        g_ui.tab = (g_ui.tab + 1) % n;
        return;
    }
    if (k == K_PREV)
    {
        g_ui.tab = (g_ui.tab + n - 1) % n;
        return;
    }
    if (k >= K_TAB1)
    {
        int idx = k - K_TAB1;
        if (idx < n)
        {
            g_ui.tab = idx;
        }
        return;
    }

    // Graph zoom works from any tab: '-' coarsens (more time per column), '=' returns toward live.
    if (k == K_ZOOM_OUT || k == K_ZOOM_IN)
    {
        g_settings.zoom += (k == K_ZOOM_OUT ? 1 : -1);
        if (g_settings.zoom < 0)
        {
            g_settings.zoom = 0;
        }
        if (g_settings.zoom >= g_zoomCount)
        {
            g_settings.zoom = g_zoomCount - 1;
        }
        g_ui.msg = std::string("Graph zoom: ") + zoomLabel();
        saveSettings();
        return;
    }

    Cat c = g_tabCats[g_ui.tab];

    // Settings tab: up/down move the selection, toggle flips it and persists.
    if (c == Cat::Settings)
    {
        int m = static_cast<int>(g_setItems.size());
        if (k == K_UP && g_ui.setSel > 0)
        {
            g_ui.setSel--;
        }
        else if (k == K_DOWN && g_ui.setSel + 1 < m)
        {
            g_ui.setSel++;
        }
        else if (k == K_TOGGLE && g_ui.setSel >= 0 && g_ui.setSel < m)
        {
            const SetItem& it = g_setItems[g_ui.setSel];
            if (it.kind == SetItem::Net)
            {
                g_settings.net[it.key] = !it.on;
            }
            else if (it.kind == SetItem::Storage)
            {
                g_settings.storage[it.key] = !it.on;
            }
            else if (it.kind == SetItem::Option && it.key == "cpu_cores")
            {
                g_settings.cpuCoreStyle = it.on ? "rows" : "grid";
            }
            else if (it.kind == SetItem::Option && it.key == "swap_scale")
            {
                g_settings.swapScale = it.on ? "ram" : "fullness";
            }
            saveSettings();
        }
        return;
    }

    // Other tabs: t cycles through the tab's views (primary load, then each thermal graph).
    if (k == K_TOGGLE)
    {
        if (c == Cat::Overview)
        {
            return; // overview has no views to cycle
        }
        int vc = g_ui.tab < static_cast<int>(g_tabViewCount.size()) ? g_tabViewCount[g_ui.tab] : 1;
        if (vc <= 1)
        {
            g_ui.msg = std::string("No thermal sensors for ") + catLabel(c);
            return;
        }
        g_ui.view[static_cast<int>(c)] = (g_ui.view[static_cast<int>(c)] + 1) % vc;
    }
}

// ---- graph rendering ------------------------------------------------------
// One column per historical sample, right-aligned (newest at the right). Each column is a
// vertical bar of eighth-blocks scaled into `height` rows and colored by its value.
// Per-column bar levels (in eighths of a full-height bar) for a history, right-aligned. -1 marks a
// column with no sample. `pct` receives the 0..100 fill fraction for coloring.
void columnLevels(const std::deque<double>& h, double vmin, double vmax, int w, int height,
                  std::vector<int>& eighths, std::vector<double>& pct)
{
    eighths.assign(w, -1);
    pct.assign(w, 0.0);
    if (vmax <= vmin)
    {
        vmax = vmin + 1;
    }
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
}

// One column per historical sample, right-aligned (newest at the right). Each column is a
// vertical bar of eighth-blocks scaled into `height` rows and colored by its value. An optional
// overlay series (e.g. swap) is drawn as a single-cell line in `ovColor` on top of the fill.
// An overlay series drawn as a single-cell line on top of the fill (e.g. swap, or the per-bucket
// peak/min lines). Each has its own axis max and color.
struct Overlay
{
    const std::deque<double>* h = nullptr;
    double max = 100.0;
    const char* color = BLUE;
};

std::vector<std::string> renderGraph(const std::deque<double>& h, double vmin, double vmax, int w,
                                     int height, Ax ax,
                                     const std::vector<Overlay>& overlays = {}, int labelW = 8)
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

    std::vector<int> eighths;
    std::vector<double> pct;
    columnLevels(h, vmin, vmax, w, height, eighths, pct);
    std::vector<std::vector<int>> ovE(overlays.size());
    for (size_t i = 0; i < overlays.size(); ++i)
    {
        std::vector<double> ovPct;
        columnLevels(*overlays[i].h, vmin, overlays[i].max, w, height, ovE[i], ovPct);
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
        std::string row = std::string(GREY) + padLeft(label, labelW) + " │" + RESET;
        for (int c = 0; c < w; ++c)
        {
            // Overlay lines: draw a marker in the cell that holds each overlay's top. Later overlays
            // win when two lines land in the same cell.
            const char* ovHit = nullptr;
            for (size_t i = 0; i < overlays.size(); ++i)
            {
                if (ovE[i][c] >= 0)
                {
                    int top = (ovE[i][c] + 7) / 8 - 1; // topmost occupied cell index from bottom
                    if (top == cellFromBottom)
                    {
                        ovHit = overlays[i].color;
                    }
                }
            }
            if (ovHit)
            {
                row += ovHit;
                row += BLOCKS[8];
                row += RESET;
                continue;
            }
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

// A compact multi-row graph with no axis/border: each row is exactly `w` display columns wide, so
// cells tile cleanly side by side. Used for the Task-Manager-style per-core grid.
std::vector<std::string> miniGraph(const std::deque<double>& h, double vmin, double vmax, int w,
                                   int height)
{
    std::vector<int> eighths;
    std::vector<double> pct;
    columnLevels(h, vmin, vmax, w, height, eighths, pct);
    std::vector<std::string> rows;
    rows.reserve(height);
    for (int L = 0; L < height; ++L)
    {
        int cellFromBottom = height - 1 - L;
        std::string row;
        for (int c = 0; c < w; ++c)
        {
            int e = eighths[c] < 0 ? 0 : eighths[c] - cellFromBottom * 8;
            e = e < 0 ? 0 : e > 8 ? 8 : e;
            if (e == 0)
            {
                row += (L == height - 1) ? std::string(TRACK) + "▁" + RESET : std::string(" ");
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

    if (cat == Cat::Mem) // absolute used bytes on a 0..total axis
    {
        const Reading* used = find(d, Quantity::DataVolume, "Used");
        const Reading* total = find(d, Quantity::DataVolume, "Total");
        if (used && total && total->value > 0)
        {
            s.ok = true;
            s.h = histFor(used);
            s.cur = used->value;
            s.vmin = 0;
            s.vmax = total->value;
            s.ax = Ax::Bytes;
        }
        return s;
    }

    const Reading* r = nullptr;
    switch (cat)
    {
        case Cat::Cpu: r = find(d, Quantity::Load, "Total"); break;
        case Cat::Gpu: r = find(d, Quantity::Load, "Core"); break;
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
    if (s.ax == Ax::Bytes)
    {
        return bytesSize(s.cur);
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

    // Storage (only devices enabled in Settings)
    bool anyDisk = false;
    for (auto& kv : devs)
    {
        if (kv.second.info && kv.second.info->id.kind == DeviceKind::Storage && showStorage(kv.second))
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
            if (!d.info || d.info->id.kind != DeviceKind::Storage || !showStorage(d))
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

    // Network: interfaces enabled in Settings (default: connected, non-virtual).
    std::vector<const DevView*> active;
    for (auto& kv : devs)
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Network)
        {
            continue;
        }
        if (showNet(d))
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

// The Settings tab: a selectable list of network interfaces and storage devices, each with a
// show/hide checkbox. Toggling persists to the YAML file next to the binary.
void renderSettings(const std::map<DeviceId, DevView>& devs, std::vector<std::string>& body)
{
    auto push = [&](const std::string& s = "") { body.push_back(s); };

    struct Item
    {
        SetItem::Kind kind;
        std::string section; // header this row lives under
        std::string key;
        std::string label;
        std::string extra;
        bool on;
    };
    std::vector<Item> items;

    // Display options first.
    bool grid = g_settings.cpuCoreStyle == "grid";
    items.push_back(
        {SetItem::Option, "Display options", "cpu_cores", "CPU per-core view",
         grid ? "grid (Task Manager)" : "rows (sparklines)", grid});
    bool swapFull = g_settings.swapScale == "fullness";
    items.push_back(
        {SetItem::Option, "Display options", "swap_scale", "Memory swap scale",
         swapFull ? "fullness (% of swap)" : "vs RAM total", swapFull});

    for (auto& kv : devs) // network
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Network)
        {
            continue;
        }
        items.push_back({SetItem::Net, "Network interfaces", d.info->name, d.info->name,
                         netLinkUp(d) ? "link up" : "link down", showNet(d)});
    }
    for (auto& kv : devs) // storage
    {
        const DevView& d = kv.second;
        if (!d.info || d.info->id.kind != DeviceKind::Storage)
        {
            continue;
        }
        std::string bsd = attr(*d.info, "bsd_name");
        items.push_back({SetItem::Storage, "Storage devices", storageKey(d), d.info->name,
                         bsd.empty() ? "" : "(" + bsd + ")", showStorage(d)});
    }

    if (g_ui.setSel >= static_cast<int>(items.size()))
    {
        g_ui.setSel = static_cast<int>(items.size()) - 1;
    }
    if (g_ui.setSel < 0)
    {
        g_ui.setSel = 0;
    }

    push(std::string("  ") + HEAD + BOLD + "SETTINGS" + RESET + GREY + "  choose what to show" +
         RESET);
    push();

    g_setItems.clear();
    int idx = 0;
    std::string section;
    for (const Item& it : items)
    {
        if (it.section != section)
        {
            if (!section.empty())
            {
                push();
            }
            push(std::string("  ") + HEAD + BOLD + it.section + RESET);
            section = it.section;
        }
        bool sel = idx == g_ui.setSel;
        std::string cursor = sel ? std::string(CYAN) + "  > " + RESET : "    ";
        std::string box = it.kind == SetItem::Choice
                              ? std::string(CYAN) + " » " + RESET
                              : (it.on ? std::string(GREEN) + "[x]" + RESET
                                       : std::string(GREY) + "[ ]" + RESET);
        std::string name = sel ? std::string(BOLD) + truncPad(it.label, 26) + RESET
                               : truncPad(it.label, 26);
        push(cursor + box + " " + name + "  " + GREY + it.extra + RESET);
        g_setItems.push_back({it.kind, it.key, it.on});
        ++idx;
    }
    push();
    push(std::string("  ") + GREY + "↑/↓ select · Space toggle · saved to " + g_configPath + RESET);
}

// The CPU per-core view: either one sparkline row per core, or a Task-Manager-style grid of mini
// graphs, per the cpu_cores setting.
void renderCores(const DevView& cpu, int cols, int gh, std::vector<std::string>& body)
{
    auto push = [&](const std::string& s = "") { body.push_back(s); };
    std::vector<const Reading*> cores;
    for (const Reading* r : cpu.readings)
    {
        if (r->quantity == Quantity::Load && r->channel.rfind("Core", 0) == 0)
        {
            cores.push_back(r);
        }
    }
    std::sort(cores.begin(), cores.end(), [](const Reading* a, const Reading* b)
              { return coreIndex(a->channel) < coreIndex(b->channel); });
    int n = static_cast<int>(cores.size());
    if (n == 0)
    {
        push(std::string("  ") + GREY + "no per-core data" + RESET);
        return;
    }

    if (g_settings.cpuCoreStyle != "grid")
    {
        int sw = std::max(10, std::min(cols - 22, 200));
        int per = columnSamples();
        for (const Reading* r : cores)
        {
            char pctbuf[12];
            std::snprintf(pctbuf, sizeof(pctbuf), "%5.1f%%", r->value);
            std::deque<double> hB = bucketHistory(histFor(r), sw, per, Agg::Avg);
            push("  " + truncPad(r->channel, 8) + " " + sparkline(hB, 0, 100, sw) + " " +
                 heatColor(r->value) + pctbuf + RESET);
        }
        return;
    }

    // Grid: choose up to 4 columns, each at least 12 display cells wide.
    const int gap = 2;
    int avail = cols - 2;
    int gcols = 1;
    for (int k = std::min(4, n); k >= 1; --k)
    {
        if ((avail - (k - 1) * gap) / k >= 12)
        {
            gcols = k;
            break;
        }
    }
    int cellW = std::max(8, (avail - (gcols - 1) * gap) / gcols);
    int gridRows = (n + gcols - 1) / gcols;
    int cellH = std::max(2, std::min(5, gh / std::max(1, gridRows) - 1));

    for (int gr = 0; gr < gridRows; ++gr)
    {
        // Label line: "Core N  42%" per cell, padded to the cell width.
        std::string labelLine = "  ";
        for (int gc = 0; gc < gcols; ++gc)
        {
            int idx = gr * gcols + gc;
            std::string cell;
            if (idx < n)
            {
                const Reading* r = cores[idx];
                char pctbuf[16];
                std::snprintf(pctbuf, sizeof(pctbuf), "%.0f%%", r->value);
                int disp = static_cast<int>(r->channel.size()) + 2 + static_cast<int>(strlen(pctbuf));
                cell = std::string(BOLD) + r->channel + RESET + "  " + heatColor(r->value) + pctbuf +
                       RESET;
                if (disp < cellW)
                {
                    cell += std::string(cellW - disp, ' ');
                }
            }
            else
            {
                cell = std::string(cellW, ' ');
            }
            labelLine += cell;
            if (gc < gcols - 1)
            {
                labelLine += std::string(gap, ' ');
            }
        }
        push(labelLine);

        // Graph lines: mini graphs composed side by side (each exactly cellW display columns).
        std::vector<std::vector<std::string>> g(gcols);
        for (int gc = 0; gc < gcols; ++gc)
        {
            int idx = gr * gcols + gc;
            g[gc] = idx < n ? miniGraph(bucketHistory(histFor(cores[idx]), cellW, columnSamples(),
                                                       Agg::Avg),
                                        0, 100, cellW, cellH)
                            : std::vector<std::string>(cellH, std::string(cellW, ' '));
        }
        for (int L = 0; L < cellH; ++L)
        {
            std::string line = "  ";
            for (int gc = 0; gc < gcols; ++gc)
            {
                line += g[gc][L];
                if (gc < gcols - 1)
                {
                    line += std::string(gap, ' ');
                }
            }
            push(line);
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
            {"t / Space", "cycle views: load → per-core (CPU) → each thermal graph"},
            {"- / =", "zoom graph out / in (seconds per column)"},
            {"Up / Down", "move selection (Settings tab)"},
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
            // Network / storage: honor the Settings show/hide choices.
            if (cat == Cat::Net && !showNet(kv.second))
            {
                continue;
            }
            if (cat == Cat::Disk && !showStorage(kv.second))
            {
                continue;
            }
            t.devs.push_back(&kv.second);
        }
        if (!t.devs.empty())
        {
            tabs.push_back(std::move(t));
        }
    }
    // Settings is always available as the last tab.
    {
        Tab st;
        st.cat = Cat::Settings;
        tabs.push_back(std::move(st));
    }

    g_tabCats.clear();
    g_tabViewCount.clear();
    for (const Tab& t : tabs)
    {
        g_tabCats.push_back(t.cat);
        int vc = 1;
        if (t.cat != Cat::Overview && t.cat != Cat::Settings)
        {
            vc = t.cat == Cat::Cpu ? 2 : 1; // CPU: overall + cores; others: one primary view
            for (const DevView* d : t.devs)
            {
                for (const Reading* r : d->readings)
                {
                    if (r->quantity == Quantity::Temperature)
                    {
                        ++vc; // one cyclable thermal graph per sensor
                    }
                }
            }
        }
        g_tabViewCount.push_back(vc);
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
    else if (tab.cat == Cat::Settings)
    {
        renderSettings(devs, body);
    }
    else
    {
        const DevView& first = *tab.devs.front();
        bool multi = tab.devs.size() > 1;

        // Build the cyclable views for this tab: primary load view(s), then one per thermal sensor.
        struct View
        {
            enum Kind
            {
                Overall,
                Cores,
                Primary,
                Temp
            } kind;
            const Reading* temp = nullptr;
            std::string label;
        };
        std::vector<View> views;
        if (tab.cat == Cat::Cpu)
        {
            views.push_back({View::Overall, nullptr, "Overall"});
            views.push_back({View::Cores, nullptr, "Cores"});
        }
        else
        {
            views.push_back({View::Primary, nullptr, "Load"});
        }
        for (const DevView* d : tab.devs)
        {
            for (const Reading* r : d->readings)
            {
                if (r->quantity == Quantity::Temperature)
                {
                    std::string lbl = "Temp: " + ((multi && d->info) ? d->info->name + " " : "") +
                                      r->channel;
                    views.push_back({View::Temp, r, lbl});
                }
            }
        }
        int& vi = g_ui.view[static_cast<int>(tab.cat)];
        if (vi >= static_cast<int>(views.size()) || vi < 0)
        {
            vi = 0;
        }
        const View& v = views[vi];

        // Tab title with the current view name (and position when there is more than one).
        std::string devName = first.info ? first.info->name : catLabel(tab.cat);
        std::string pos = views.size() > 1 ? std::string("   ") + GREY + "(" +
                                                 std::to_string(vi + 1) + "/" +
                                                 std::to_string(views.size()) + ")" + RESET
                                           : std::string();
        push(std::string("  ") + HEAD + BOLD + catLabel(tab.cat) + RESET + "  " + devName + "   " +
             GREY + "[ " + RESET + CYAN + v.label + RESET + GREY + " ]" + RESET + pos);
        push();

        if (v.kind == View::Temp)
        {
            // Thermal history graph for this one sensor.
            std::deque<double> hB = bucketHistory(histFor(v.temp), gw, columnSamples(), Agg::Avg);
            std::vector<std::string> g = renderGraph(hB, 0, 100, gw, gh, Ax::Temp);
            for (std::string& row : g)
            {
                push("  " + row);
            }
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f°C", v.temp->value);
            push(std::string("  ") + std::string(9, ' ') + GREY + "now " + RESET + b +
                 std::string(GREY) + "  zoom " + RESET + zoomLabel());
        }
        else if (v.kind == View::Cores)
        {
            renderCores(first, cols, gh, body);
        }
        else if (!multi)
        {
            // Single device: full-height graph plus a stat line. Memory overlays swap in blue.
            Series s = seriesFor(tab.cat, first, false);
            std::deque<double> swapRaw; // memory-only swap series (raw, pre-bucketing)
            double swapMax = s.vmax;
            bool hasSwap = false;
            if (tab.cat == Cat::Mem)
            {
                // Swap overlay scale is a setting: "fullness" (swap used vs its own total, so it
                // stays visible even though swap is tiny next to RAM) or "ram" (absolute bytes on
                // the same axis as usage).
                const Reading* swapUsed = find(first, Quantity::DataVolume, "Swap Used");
                if (g_settings.swapScale == "ram")
                {
                    if (swapUsed)
                    {
                        for (double bytes : histFor(swapUsed))
                        {
                            swapRaw.push_back(bytes);
                        }
                        swapMax = s.vmax; // share the RAM byte axis
                        hasSwap = true;
                    }
                }
                else
                {
                    const Reading* swapTotal = find(first, Quantity::DataVolume, "Swap Total");
                    if (swapUsed && swapTotal && swapTotal->value > 0)
                    {
                        for (double bytes : histFor(swapUsed))
                        {
                            swapRaw.push_back(bytes / swapTotal->value);
                        }
                        swapMax = 1.0; // 0..1 fullness fills the graph height
                        hasSwap = true;
                    }
                }
            }
            // Size the left axis gutter to the widest tick label so rate labels
            // like "125.0 MB/s" don't overrun the reserved margin and wrap the line.
            int lw = static_cast<int>(axLabel(s.ax, s.vmax).size());
            lw = std::max(lw, static_cast<int>(axLabel(s.ax, (s.vmin + s.vmax) / 2).size()));
            lw = std::max(lw, static_cast<int>(axLabel(s.ax, s.vmin).size()));
            lw = std::max(lw, 3);
            int ggw = std::max(20, std::min(cols - (4 + lw), 240)); // graph width for this gutter
            // Apply the zoom: bucket the raw 1 s history into the graph columns (live == raw).
            int per = columnSamples();
            std::deque<double> hB = bucketHistory(s.h, ggw, per, Agg::Avg);
            std::deque<double> swapB;
            std::vector<Overlay> ovs;
            if (hasSwap)
            {
                swapB = bucketHistory(swapRaw, ggw, per, Agg::Avg);
                ovs.push_back({&swapB, swapMax, BLUE});
            }
            std::vector<std::string> g =
                renderGraph(hB, s.vmin, s.vmax, ggw, gh, s.ax, ovs, lw);
            for (std::string& row : g)
            {
                push("  " + row);
            }
            std::string stat = std::string("  ") + std::string(lw, ' ') + GREY + "now " + RESET +
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
            if (tab.cat == Cat::Mem)
            {
                const Reading* total = find(first, Quantity::DataVolume, "Total");
                const Reading* use = find(first, Quantity::Load, "Usage");
                if (total)
                {
                    stat += std::string(GREY) + " / " + RESET + bytesSize(total->value);
                }
                if (use)
                {
                    char b[16];
                    std::snprintf(b, sizeof(b), "%.0f%%", use->value);
                    stat += std::string(GREY) + "  (" + RESET + b + GREY + ")" + RESET;
                }
            }
            if (hasSwap)
            {
                const Reading* swapUsed = find(first, Quantity::DataVolume, "Swap Used");
                const Reading* swapTotal = find(first, Quantity::DataVolume, "Swap Total");
                stat += std::string(GREY) + "  swap " + RESET + BLUE +
                        bytesSize(swapUsed ? swapUsed->value : 0.0) + RESET;
                if (swapTotal)
                {
                    stat += std::string(GREY) + " / " + RESET + bytesSize(swapTotal->value);
                }
            }
            stat += std::string(GREY) + "  zoom " + RESET + zoomLabel();
            push(stat);
        }
        else
        {
            // Multiple devices in one category: one sparkline row each.
            int sw = std::max(10, std::min(cols - 32, 200));
            for (const DevView* d : tab.devs)
            {
                Series s = seriesFor(tab.cat, *d, false);
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
                       "n/p tab   t toggle   -/= zoom   1-9 jump   h help   q quit" + RESET;

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
    g_interval = interval;

    std::signal(SIGINT, onSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, onSignal);
#endif
    g_configPath = exeDir() + "/idimus_monitor.yaml";
    loadSettings(g_configPath);

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
