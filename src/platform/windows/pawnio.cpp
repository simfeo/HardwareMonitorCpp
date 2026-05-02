// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "platform/windows/pawnio.hpp"

#ifdef _WIN32

#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

namespace idimus_hw {
namespace win {
namespace {

// Official PawnIOLib signatures (from PawnIOLib.h).
using OpenFn = HRESULT(STDAPICALLTYPE*)(PHANDLE);
using LoadFn = HRESULT(STDAPICALLTYPE*)(HANDLE, const UCHAR*, SIZE_T);
using ExecFn = HRESULT(STDAPICALLTYPE*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T,
                                        PSIZE_T);
using CloseFn = HRESULT(STDAPICALLTYPE*)(HANDLE);

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

// Search order for a signed module: $IDIMUS_PAWNIO_DIR, <exe>\modules, <exe>.
std::wstring findModule(const std::wstring& fileName) {
    std::vector<std::wstring> dirs;
    wchar_t env[1024];
    DWORD n = GetEnvironmentVariableW(L"IDIMUS_PAWNIO_DIR", env, 1024);
    if (n > 0 && n < 1024)
        dirs.emplace_back(env, n);
    std::wstring ed = exeDir();
    if (!ed.empty()) {
        dirs.push_back(ed + L"\\modules");
        dirs.push_back(ed);
    }
    for (const std::wstring& d : dirs) {
        std::wstring path = d + L"\\" + fileName;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    return {};
}

} // namespace

PawnIo::PawnIo() {
    // PawnIOLib.dll installs under %ProgramFiles%\PawnIO, which is not on the default DLL search
    // path — try the install location(s) as well as the bare name.
    lib_ = LoadLibraryW(L"PawnIOLib.dll");
    if (!lib_) {
        wchar_t pf[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) > 0) {
            std::wstring p = std::wstring(pf) + L"\\PawnIO\\PawnIOLib.dll";
            lib_ = LoadLibraryW(p.c_str());
        }
    }
    if (!lib_)
        lib_ = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib_)
        return;
    auto mod = static_cast<HMODULE>(lib_);
    open_ = reinterpret_cast<void*>(GetProcAddress(mod, "pawnio_open"));
    load_ = reinterpret_cast<void*>(GetProcAddress(mod, "pawnio_load"));
    exec_ = reinterpret_cast<void*>(GetProcAddress(mod, "pawnio_execute"));
    close_ = reinterpret_cast<void*>(GetProcAddress(mod, "pawnio_close"));
    if (!open_ || !load_ || !exec_ || !close_)
        return;

    HANDLE h = nullptr;
    if (reinterpret_cast<OpenFn>(open_)(&h) == S_OK && h) {
        handle_ = h;
        ready_ = true; // driver present and executor open
    }
}

PawnIo::~PawnIo() {
    if (handle_ && close_)
        reinterpret_cast<CloseFn>(close_)(handle_);
    if (lib_)
        FreeLibrary(static_cast<HMODULE>(lib_));
}

bool PawnIo::loadModule(const std::string& baseName) {
    if (!ready_)
        return false;
    std::wstring file(baseName.begin(), baseName.end());
    file += L".bin";
    std::wstring path = findModule(file);
    if (path.empty())
        return false;

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<unsigned char> blob((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (blob.empty())
        return false;

    if (reinterpret_cast<LoadFn>(load_)(handle_, blob.data(), blob.size()) != S_OK)
        return false;
    moduleLoaded_ = true;
    return true;
}

bool PawnIo::execute(const char* fn, const uint64_t* in, size_t inN, uint64_t* out, size_t outN) {
    if (!ready_)
        return false;
    SIZE_T got = 0;
    HRESULT hr = reinterpret_cast<ExecFn>(exec_)(handle_, fn, in, inN, out, outN, &got);
    return hr == S_OK && got >= outN;
}

bool PawnIo::readMsr(uint32_t msr, uint64_t& value) {
    if (!moduleLoaded_)
        return false;
    uint64_t in = msr;
    uint64_t out = 0;
    if (!execute("ioctl_read_msr", &in, 1, &out, 1))
        return false;
    value = out;
    return true;
}

} // namespace win
} // namespace idimus_hw

#endif // _WIN32
