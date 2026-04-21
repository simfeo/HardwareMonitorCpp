// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "idimus_hw/snapshot.hpp"

namespace idimus_hw {

std::vector<Reading> Snapshot::forDevice(const DeviceId& id) const {
    std::vector<Reading> out;
    for (const Reading& r : readings_)
        if (r.device == id)
            out.push_back(r);
    return out;
}

std::vector<Reading> Snapshot::forQuantity(Quantity q) const {
    std::vector<Reading> out;
    for (const Reading& r : readings_)
        if (r.quantity == q)
            out.push_back(r);
    return out;
}

const DeviceInfo* Snapshot::device(const DeviceId& id) const {
    for (const DeviceInfo& d : devices_)
        if (d.id == id)
            return &d;
    return nullptr;
}

} // namespace idimus_hw
