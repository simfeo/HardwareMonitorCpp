// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/network/win_network_source.hpp"

#ifdef _WIN32

#include <chrono>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "platform/windows/win_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Match the set a typical NIC view shows: skip loopback / tunnel / "other" types and the
// NDIS lightweight-filter pseudo-interfaces.
bool wanted(const MIB_IF_ROW2& r) {
    if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.Type == IF_TYPE_TUNNEL || r.Type == IF_TYPE_OTHER)
        return false;
    if (r.InterfaceAndOperStatusFlags.FilterInterface)
        return false;
    return true;
}
} // namespace

std::vector<DeviceInfo> WinNetworkSource::discover() {
    std::vector<DeviceInfo> result;
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return result;
    int ordinal = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (!wanted(r))
            continue;
        uint64_t luid = r.InterfaceLuid.Value;
        if (ordinals_.count(luid))
            continue;
        ordinals_[luid] = ordinal;
        DeviceInfo info;
        info.id = DeviceId{DeviceKind::Network, ordinal};
        info.name = win::wideToUtf8(r.Alias);
        if (info.name.empty())
            info.name = win::wideToUtf8(r.Description);
        info.attributes["description"] = win::wideToUtf8(r.Description);
        info.attributes["link_speed_bps"] = std::to_string(r.TransmitLinkSpeed);
        result.push_back(std::move(info));
        ++ordinal;
    }
    FreeMibTable(table);
    return result;
}

void WinNetworkSource::sample(std::vector<Reading>& out) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return;
    double t = now();
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (!wanted(r))
            continue;
        uint64_t luid = r.InterfaceLuid.Value;
        auto it = ordinals_.find(luid);
        if (it == ordinals_.end())
            continue;
        DeviceId dev{DeviceKind::Network, it->second};
        uint64_t rx = r.InOctets, tx = r.OutOctets;
        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Received", double(rx)});
        out.push_back(Reading{dev, Quantity::DataVolume, Unit::Byte, "Sent", double(tx)});
        auto pit = prev_.find(luid);
        if (pit != prev_.end()) {
            double dt = t - pit->second.t;
            if (dt > 0) {
                double down = rx >= pit->second.rx ? (rx - pit->second.rx) / dt : 0.0;
                double up = tx >= pit->second.tx ? (tx - pit->second.tx) / dt : 0.0;
                out.push_back(Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Download", down});
                out.push_back(Reading{dev, Quantity::DataRate, Unit::BytePerSecond, "Upload", up});
            }
        }
        prev_[luid] = Prev{rx, tx, t};
    }
    FreeMibTable(table);
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
