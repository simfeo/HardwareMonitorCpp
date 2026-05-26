// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
//
// Small inline helpers for reading Linux sysfs / procfs entries.
#pragma once

#ifdef __linux__

#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

namespace idimus_hw {
namespace lnx {

inline std::string readTrim(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

inline bool readU64(const std::string& path, uint64_t& out) {
    std::string s = readTrim(path);
    if (s.empty())
        return false;
    out = std::strtoull(s.c_str(), nullptr, 10);
    return true;
}

inline bool readI64(const std::string& path, int64_t& out) {
    std::string s = readTrim(path);
    if (s.empty())
        return false;
    out = std::strtoll(s.c_str(), nullptr, 10);
    return true;
}

inline std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> names;
    DIR* d = opendir(path.c_str());
    if (!d)
        return names;
    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..")
            names.push_back(n);
    }
    closedir(d);
    return names;
}

inline bool exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace lnx
} // namespace idimus_hw

#endif // __linux__
