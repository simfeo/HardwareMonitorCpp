// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/network/mac_network_source.hpp"

#ifdef __APPLE__

#include <chrono>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

namespace idimus_hw
{
namespace sources
{
namespace
{
double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Real link-layer NICs only: drop loopback and tunnel/other pseudo-types.
bool wanted(const struct ifaddrs* ifa)
{
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK)
    {
        return false;
    }
    if (ifa->ifa_flags & IFF_LOOPBACK)
    {
        return false;
    }
    auto* sdl = reinterpret_cast<const struct sockaddr_dl*>(ifa->ifa_addr);
    uint8_t t = sdl->sdl_type;
    return t != IFT_OTHER && t != IFT_GIF && t != IFT_STF && t != IFT_LOOP;
}
} // namespace

std::vector<DeviceInfo> MacNetworkSource::discover()
{
    std::vector<DeviceInfo> result;
    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || !head)
    {
        return result;
    }
    int ordinal = 0;
    for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next)
    {
        if (!wanted(ifa) || !ifa->ifa_name)
        {
            continue;
        }
        std::string name = ifa->ifa_name;
        if (ordinals_.count(name))
        {
            continue;
        }
        ordinals_[name] = ordinal;
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::Network, ordinal};
        info.name = name;
        result.push_back(info);
        ++ordinal;
    }
    freeifaddrs(head);
    return result;
}

void MacNetworkSource::sample(std::vector<Reading>& out)
{
    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || !head)
    {
        return;
    }
    double t = now();
    for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next)
    {
        if (!wanted(ifa) || !ifa->ifa_name)
        {
            continue;
        }
        std::string name = ifa->ifa_name;
        auto it = ordinals_.find(name);
        if (it == ordinals_.end())
        {
            continue;
        }
        auto* data = reinterpret_cast<const struct if_data*>(ifa->ifa_data);
        if (!data)
        {
            continue;
        }
        DeviceId dev{DeviceKind::Network, it->second};
        // Live link state (like Task Manager's show/hide): interface up and running.
        bool connected = (ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING);
        out.push_back(Reading{dev, Quantity::Count, Unit::None, "Link", connected ? 1.0 : 0.0});
        uint64_t rx = data->ifi_ibytes, tx = data->ifi_obytes;

        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Received", double(rx)});
        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Sent", double(tx)});

        auto pit = prev_.find(name);
        if (pit != prev_.end())
        {
            double dt = t - pit->second.t;
            if (dt > 0)
            {
                double down = rx >= pit->second.rx ? (rx - pit->second.rx) / dt : 0.0;
                double up = tx >= pit->second.tx ? (tx - pit->second.tx) / dt : 0.0;
                out.push_back(
                    Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Download", down});
                out.push_back(Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Upload", up});
            }
        }
        prev_[name] = Prev{rx, tx, t};
    }
    freeifaddrs(head);
}

} // namespace sources
} // namespace idimus_hw

#endif // __APPLE__
