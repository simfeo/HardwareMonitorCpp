// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/gpu/linux_drm_gpu_source.hpp"

#ifdef __linux__

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw {
namespace sources {
namespace {

constexpr unsigned kAmd = 0x1002;
constexpr unsigned kIntel = 0x8086;
constexpr unsigned kNvidia = 0x10DE;
constexpr int kDrmOrdinalBase = 100; // distinct from NVML (0)

bool isBaseCard(const std::string& n) {
    if (n.rfind("card", 0) != 0 || n.size() <= 4)
        return false;
    for (size_t i = 4; i < n.size(); ++i)
        if (!std::isdigit((unsigned char)n[i]))
            return false; // skip connector nodes like card0-DP-1
    return true;
}

std::string findHwmon(const std::string& devicePath) {
    std::string base = devicePath + "/hwmon";
    for (const std::string& h : lnx::listDir(base))
        if (h.rfind("hwmon", 0) == 0)
            return base + "/" + h;
    return {};
}

} // namespace

std::vector<DeviceInfo> LinuxDrmGpuSource::discover() {
    std::vector<DeviceInfo> result;
    std::vector<std::string> cards = lnx::listDir("/sys/class/drm");
    std::sort(cards.begin(), cards.end());
    int ordinal = 0;
    for (const std::string& c : cards) {
        if (!isBaseCard(c))
            continue;
        std::string devicePath = "/sys/class/drm/" + c + "/device";
        std::string vstr = lnx::readTrim(devicePath + "/vendor");
        unsigned vendor = vstr.empty() ? 0 : unsigned(std::strtoul(vstr.c_str(), nullptr, 16));
        if (vendor != kAmd && vendor != kIntel)
            continue; // NVIDIA via NVML; others unsupported here

        Card card;
        card.id = DeviceId{vendor == kIntel ? DeviceKind::GpuIntegrated : DeviceKind::GpuDiscrete,
                           kDrmOrdinalBase + ordinal};
        card.cardName = c;
        card.devicePath = devicePath;
        card.hwmonPath = findHwmon(devicePath);
        card.vendor = vendor;
        cards_.push_back(card);

        DeviceInfo info;
        info.id = card.id;
        info.vendor = (vendor == kIntel) ? "Intel" : "AMD";
        info.name = info.vendor + " GPU";
        result.push_back(std::move(info));
        ++ordinal;
    }
    return result;
}

void LinuxDrmGpuSource::sample(std::vector<Reading>& out) {
    for (const Card& c : cards_) {
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
            out.push_back(Reading{c.id, q, u, ch, v});
        };
        int64_t v = 0;

        if (lnx::readI64(c.devicePath + "/gpu_busy_percent", v))
            emit(Quantity::Load, Unit::Percent, "Core", double(v));

        if (c.vendor == kAmd) {
            uint64_t used = 0, total = 0;
            if (lnx::readU64(c.devicePath + "/mem_info_vram_used", used))
                emit(Quantity::DataVolume, Unit::Byte, "Memory Used", double(used));
            if (lnx::readU64(c.devicePath + "/mem_info_vram_total", total))
                emit(Quantity::DataVolume, Unit::Byte, "Memory Total", double(total));
        }
        if (c.vendor == kIntel) {
            if (lnx::readI64("/sys/class/drm/" + c.cardName + "/gt_cur_freq_mhz", v))
                emit(Quantity::Clock, Unit::Megahertz, "Core", double(v));
        }

        if (!c.hwmonPath.empty()) {
            struct {
                const char* file;
                const char* label;
            } temps[] = {{"/temp1_input", "Edge"}, {"/temp2_input", "Hotspot"}, {"/temp3_input", "Memory"}};
            for (auto& t : temps) {
                int64_t milli = 0;
                if (lnx::readI64(c.hwmonPath + t.file, milli) && milli > 0)
                    emit(Quantity::Temperature, Unit::Celsius, t.label, milli / 1000.0);
            }
            if (lnx::readI64(c.hwmonPath + "/fan1_input", v) && v >= 0)
                emit(Quantity::FanSpeed, Unit::Rpm, "Fan", double(v));
            if (lnx::readI64(c.hwmonPath + "/power1_average", v) && v > 0)
                emit(Quantity::Power, Unit::Watt, "Power", v / 1e6); // µW -> W
            uint64_t hz = 0;
            if (lnx::readU64(c.hwmonPath + "/freq1_input", hz) && hz > 0)
                emit(Quantity::Clock, Unit::Megahertz, "Core", hz / 1e6); // Hz -> MHz
            if (lnx::readU64(c.hwmonPath + "/freq2_input", hz) && hz > 0)
                emit(Quantity::Clock, Unit::Megahertz, "Memory", hz / 1e6);
        }
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
