// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "idimus_hw/monitor.hpp"

namespace idimus_hw
{

void Monitor::add(std::unique_ptr<Source> source)
{
    if (source)
    {
        sources_.push_back(std::move(source));
    }
}

void Monitor::addPlatformSources()
{
    for (auto& s : createPlatformSources())
    {
        add(std::move(s));
    }
}

void Monitor::open()
{
    devices_.clear();
    for (auto& s : sources_)
    {
        for (auto& d : s->discover())
        {
            devices_.push_back(std::move(d));
        }
    }
    opened_ = true;
}

Snapshot Monitor::poll()
{
    if (!opened_)
    {
        open();
    }
    std::vector<Reading> readings;
    for (auto& s : sources_)
    {
        s->sample(readings);
    }
    return Snapshot(devices_, std::move(readings));
}

} // namespace idimus_hw
