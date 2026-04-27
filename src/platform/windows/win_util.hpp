// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Small inline Win32 helpers shared by the Windows sources.
#pragma once

#ifdef _WIN32

#include <string>

#include <windows.h>

namespace idimus_hw {
namespace win {

inline std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w)
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// Reads a REG_SZ value; empty on failure.
inline std::string regString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return wideToUtf8(buf);
    return {};
}

} // namespace win
} // namespace idimus_hw

#endif // _WIN32
