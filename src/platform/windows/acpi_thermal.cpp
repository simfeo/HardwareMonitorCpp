// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "platform/windows/acpi_thermal.hpp"

#ifdef _WIN32

#include <windows.h>
#include <wbemidl.h>

namespace hardware_monitor_cpp
{
namespace win
{

AcpiThermal::~AcpiThermal()
{
    if (svc_)
    {
        svc_->Release();
    }
    if (comInit_)
    {
        CoUninitialize();
    }
}

bool AcpiThermal::connect()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
    {
        comInit_ = true;
    }
    else if (hr != RPC_E_CHANGED_MODE)
    {
        return false;
    }

    // Process-wide and one-shot: fails with RPC_E_TOO_LATE when the host already set it, which is
    // fine - we then use whatever blanket the host chose.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

    IWbemLocator* loc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                          reinterpret_cast<void**>(&loc));
    if (FAILED(hr) || !loc)
    {
        return false;
    }
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc_);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr) || !svc_)
    {
        svc_ = nullptr;
        return false;
    }
    CoSetProxyBlanket(svc_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

bool AcpiThermal::sample(double& celsius)
{
    if (failed_)
    {
        return false;
    }
    if (!svc_ && !connect())
    {
        failed_ = true;
        return false;
    }

    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = svc_->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 nullptr, &en);
    SysFreeString(query);
    SysFreeString(lang);
    if (FAILED(hr) || !en)
    {
        return false;
    }

    double best = -1;
    IWbemClassObject* obj = nullptr;
    ULONG got = 0;
    while (en->Next(WBEM_INFINITE, 1, &obj, &got) == S_OK && got == 1)
    {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &v, nullptr, nullptr)))
        {
            double deciKelvin = -1;
            if (v.vt == VT_I4)
            {
                deciKelvin = double(v.lVal);
            }
            else if (v.vt == VT_UI4)
            {
                deciKelvin = double(v.ulVal);
            }
            double c = deciKelvin / 10.0 - 273.15;
            if (deciKelvin >= 0 && c > 0 && c < 150 && c > best)
            {
                best = c;
            }
        }
        VariantClear(&v);
        obj->Release();
        obj = nullptr;
    }
    en->Release();

    if (best < 0)
    {
        return false;
    }
    celsius = best;
    return true;
}

} // namespace win
} // namespace hardware_monitor_cpp

#endif // _WIN32
