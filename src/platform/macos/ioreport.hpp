// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Power + active-frequency reader built on the IOReport interface (libIOReport) and the device
// tree DVFS tables. Uses only documented-by-behaviour interface facts; loaded dynamically.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace idimus_hw {
namespace mac {

class IoReport {
public:
    IoReport();
    ~IoReport();
    IoReport(const IoReport&) = delete;
    IoReport& operator=(const IoReport&) = delete;

    bool ok() const { return ok_; }

    struct Sample {
        std::map<std::string, double> powerW;  // "CPU", "GPU", "ANE"
        std::map<std::string, double> freqMhz; // "E-CPU", "P-CPU", "GPU"
    };

    // Computes averages over the interval since the previous call. First call primes the
    // baseline and returns false.
    bool sample(Sample& out);

private:
    void readDvfs();

    bool ok_ = false;
    void* lib_ = nullptr;
    void* sub_ = nullptr;
    void* chans_ = nullptr;
    void* prev_ = nullptr;
    double prevT_ = 0;

    std::vector<double> eCpuMhz_; // voltage-states1
    std::vector<double> pCpuMhz_; // voltage-states5
    std::vector<double> gpuMhz_;  // voltage-states9 (index 0 = idle/0)
};

} // namespace mac
} // namespace idimus_hw
