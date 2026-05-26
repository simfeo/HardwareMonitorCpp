// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/network/linux_network_source.hpp"

#ifdef __linux__

#include <algorithm>
#include <chrono>

#include "platform/linux/sysfs.hpp"

namespace idimus_hw {
namespace sources {
namespace {
constexpr int ARPHRD_LOOPBACK = 772;
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool isRealNic(const std::string& name) {
    if (name == "lo")
        return false;
    int64_t type = 0;
    if (lnx::readI64("/sys/class/net/" + name + "/type", type) && type == ARPHRD_LOOPBACK)
        return false;
    return lnx::exists("/sys/class/net/" + name + "/statistics/rx_bytes");
}
} // namespace

std::vector<DeviceInfo> LinuxNetworkSource::discover() {
    std::vector<DeviceInfo> result;
    std::vector<std::string> names = lnx::listDir("/sys/class/net");
    std::sort(names.begin(), names.end());
    int ordinal = 0;
    for (const std::string& n : names) {
        if (!isRealNic(n))
            continue;
        ordinals_[n] = ordinal;
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::Network, ordinal};
        info.name = n;
        int64_t speed = 0;
        if (lnx::readI64("/sys/class/net/" + n + "/speed", speed) && speed > 0)
            info.attributes["link_speed_mbps"] = std::to_string(speed);
        result.push_back(std::move(info));
        ++ordinal;
    }
    return result;
}

void LinuxNetworkSource::sample(std::vector<Reading>& out) {
    double t = nowSeconds();
    for (const auto& kv : ordinals_) {
        const std::string& n = kv.first;
        DeviceId dev{DeviceKind::Network, kv.second};
        uint64_t rx = 0, tx = 0;
        lnx::readU64("/sys/class/net/" + n + "/statistics/rx_bytes", rx);
        lnx::readU64("/sys/class/net/" + n + "/statistics/tx_bytes", tx);
        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Received", double(rx)});
        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Sent", double(tx)});
        auto pit = prev_.find(n);
        if (pit != prev_.end()) {
            double dt = t - pit->second.t;
            if (dt > 0) {
                double down = rx >= pit->second.rx ? (rx - pit->second.rx) / dt : 0.0;
                double up = tx >= pit->second.tx ? (tx - pit->second.tx) / dt : 0.0;
                out.push_back(Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Download", down});
                out.push_back(Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Upload", up});
            }
        }
        prev_[n] = Prev{rx, tx, t};
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // __linux__
