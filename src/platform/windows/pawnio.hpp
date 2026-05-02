// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Ring-0 access via PawnIO's official user-mode library (PawnIOLib.dll), loaded dynamically.
// Used to read model-specific registers through the signed IntelMSR module. Windows only.
//
// The signed module blob (IntelMSR.bin) is NOT bundled: it is a release artifact of the pinned
// PawnIO.Modules submodule (LGPL-2.1). Place it in a `modules/` folder next to the executable or
// point the IDIMUS_PAWNIO_DIR environment variable at it.
#pragma once

#include <cstdint>
#include <string>

namespace idimus_hw {
namespace win {

class PawnIo {
public:
    PawnIo();
    ~PawnIo();
    PawnIo(const PawnIo&) = delete;
    PawnIo& operator=(const PawnIo&) = delete;

    bool ok() const { return ready_; }

    // Locates and loads a module .bin by base name (e.g. "IntelMSR"); returns false if missing.
    bool loadModule(const std::string& baseName);

    // Reads an MSR via the IntelMSR module's ioctl_read_msr. False if the read is denied/fails.
    bool readMsr(uint32_t msr, uint64_t& value);

private:
    bool execute(const char* fn, const uint64_t* in, size_t inN, uint64_t* out, size_t outN);

    void* lib_ = nullptr;    // PawnIOLib.dll
    void* handle_ = nullptr; // PawnIO executor handle
    void* open_ = nullptr;
    void* load_ = nullptr;
    void* exec_ = nullptr;
    void* close_ = nullptr;
    bool ready_ = false;
    bool moduleLoaded_ = false;
};

} // namespace win
} // namespace idimus_hw
