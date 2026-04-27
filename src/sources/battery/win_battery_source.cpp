// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 idimus. Free for non-commercial use; commercial use requires a license.
#include "sources/battery/win_battery_source.hpp"

#ifdef _WIN32

#include <vector>

#include <windows.h>
#include <winioctl.h> // CTL_CODE used by the IOCTL_BATTERY_* macros in <batclass.h>
#include <batclass.h>
#include <setupapi.h>

#include "platform/windows/win_util.hpp"

namespace idimus_hw {
namespace sources {
namespace {

// Battery device interface class GUID {72631e54-78A4-11d0-bcf7-00aa00b7b32a}. Declared locally
// to avoid pulling <initguid.h> (which would emit every SDK GUID and collide).
const GUID kGuidBattery = {0x72631e54, 0x78A4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

std::string queryString(HANDLE h, uint32_t tag, BATTERY_QUERY_INFORMATION_LEVEL level) {
    BATTERY_QUERY_INFORMATION bqi{};
    bqi.BatteryTag = tag;
    bqi.InformationLevel = level;
    wchar_t buf[128] = {};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi), buf,
                        sizeof(buf) - sizeof(wchar_t), &got, nullptr))
        return win::wideToUtf8(buf);
    return {};
}

} // namespace

WinBatterySource::~WinBatterySource() {
    for (auto& b : batteries_)
        if (b.handle)
            CloseHandle(static_cast<HANDLE>(b.handle));
}

std::vector<DeviceInfo> WinBatterySource::discover() {
    std::vector<DeviceInfo> result;
    HDEVINFO dev = SetupDiGetClassDevsW(&kGuidBattery, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE)
        return result;

    int ordinal = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(dev, nullptr, &kGuidBattery, i, &ifd)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            continue;
        }
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(dev, &ifd, nullptr, 0, &need, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || need == 0)
            continue;
        std::vector<char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(dev, &ifd, detail, need, &need, nullptr))
            continue;

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        bool kept = false;
        DWORD wait = 0, got = 0;
        uint32_t tag = 0;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag),
                            &got, nullptr)) {
            BATTERY_QUERY_INFORMATION bqi{};
            bqi.BatteryTag = tag;
            bqi.InformationLevel = BatteryInformation;
            BATTERY_INFORMATION bi{};
            if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi), &bi,
                                sizeof(bi), &got, nullptr) &&
                (bi.Capabilities & BATTERY_SYSTEM_BATTERY)) {
                Bat b;
                b.id = DeviceId{DeviceKind::Battery, ordinal};
                b.handle = h;
                b.tag = tag;
                b.designCapacity = bi.DesignedCapacity;
                b.fullChargeCapacity = bi.FullChargedCapacity;
                batteries_.push_back(b);

                DeviceInfo info;
                info.id = b.id;
                std::string name = queryString(h, tag, BatteryDeviceName);
                info.name = name.empty() ? "Battery" : name;
                info.vendor = queryString(h, tag, BatteryManufactureName);
                result.push_back(std::move(info));
                kept = true;
                ++ordinal;
            }
        }
        if (!kept)
            CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(dev);
    return result;
}

void WinBatterySource::sample(std::vector<Reading>& out) {
    for (auto& b : batteries_) {
        HANDLE h = static_cast<HANDLE>(b.handle);
        auto emit = [&](Quantity q, Unit u, const std::string& ch, double v) {
            out.push_back(Reading{b.id, q, u, ch, v});
        };

        if (b.designCapacity != BATTERY_UNKNOWN_CAPACITY)
            emit(Quantity::Capacity, Unit::MilliwattHour, "Design Capacity", b.designCapacity);
        if (b.fullChargeCapacity != BATTERY_UNKNOWN_CAPACITY) {
            emit(Quantity::Capacity, Unit::MilliwattHour, "Full Charge Capacity", b.fullChargeCapacity);
            if (b.designCapacity != BATTERY_UNKNOWN_CAPACITY && b.designCapacity > 0)
                emit(Quantity::Level, Unit::Percent, "Health",
                     100.0 * double(b.fullChargeCapacity) / double(b.designCapacity));
        }

        BATTERY_WAIT_STATUS bws{};
        bws.BatteryTag = b.tag;
        BATTERY_STATUS st{};
        DWORD got = 0;
        double voltageV = 0;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws), &st, sizeof(st), &got,
                            nullptr)) {
            if (st.Capacity != BATTERY_UNKNOWN_CAPACITY) {
                emit(Quantity::Capacity, Unit::MilliwattHour, "Remaining Capacity", st.Capacity);
                if (b.fullChargeCapacity != BATTERY_UNKNOWN_CAPACITY && b.fullChargeCapacity > 0)
                    emit(Quantity::Level, Unit::Percent, "Charge",
                         100.0 * double(st.Capacity) / double(b.fullChargeCapacity));
            }
            if (st.Voltage != BATTERY_UNKNOWN_VOLTAGE) {
                voltageV = st.Voltage / 1000.0;
                emit(Quantity::Voltage, Unit::Volt, "Voltage", voltageV);
            }
            if (uint32_t(st.Rate) != BATTERY_UNKNOWN_RATE) {
                double rateW = st.Rate / 1000.0; // signed: + charging, - discharging
                emit(Quantity::Power, Unit::Watt, "Power", rateW < 0 ? -rateW : rateW);
                if (voltageV > 0)
                    emit(Quantity::Current, Unit::Ampere, "Current",
                         (rateW < 0 ? -rateW : rateW) / voltageV);
            }
        }

        BATTERY_QUERY_INFORMATION bqi{};
        bqi.BatteryTag = b.tag;
        ULONG seconds = 0;
        bqi.InformationLevel = BatteryEstimatedTime;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi), &seconds,
                            sizeof(seconds), &got, nullptr) &&
            seconds != BATTERY_UNKNOWN_TIME)
            emit(Quantity::Duration, Unit::Second, "Runtime", double(seconds));

        ULONG temp = 0;
        bqi.InformationLevel = BatteryTemperature;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi), &temp,
                            sizeof(temp), &got, nullptr) &&
            temp != 0)
            emit(Quantity::Temperature, Unit::Celsius, "Temperature", temp / 10.0 - 273.15);
    }
}

} // namespace sources
} // namespace idimus_hw

#endif // _WIN32
