// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Minimal AppleSMC key reader (IOKit). The SMC key protocol is an undocumented but fixed
// hardware interface; only those interface facts are used here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace idimus_hw
{
namespace mac
{

class Smc
{
public:
    Smc();
    ~Smc();
    Smc(const Smc&) = delete;
    Smc& operator=(const Smc&) = delete;

    bool ok() const
    {
        return conn_ != 0;
    }

    // Reads a 4-char key and converts to float per its data type. False on any failure.
    bool readFloat(const std::string& key, float& out);

    // All key names known to the controller (slow; enumerate once and keep what you need).
    std::vector<std::string> allKeys();

private:
    unsigned int conn_ = 0; // io_connect_t
};

} // namespace mac
} // namespace idimus_hw
