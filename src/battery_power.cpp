#include "battery_power.h"

#include "win32.h"
#include "win32_raii.h"

#include <winioctl.h> // CTL_CODE, which batclass.h builds its IOCTLs from

#include <batclass.h>
#include <cfgmgr32.h>

#include <cwchar>
#include <string>
#include <utility>

namespace bt {
namespace {

// GUID_DEVICE_BATTERY from batclass.h, spelled out rather than pulled in with
// initguid.h: that header emits a definition for every GUID the Windows headers
// declare, and main.cpp already instantiates the power setting ones.
constexpr GUID kBatteryInterface = {
    0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

bool query_information(HANDLE device, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level, void* out,
                       DWORD size) {
    BATTERY_QUERY_INFORMATION request{};
    request.BatteryTag = tag;
    request.InformationLevel = level;
    DWORD returned = 0;
    return DeviceIoControl(device, IOCTL_BATTERY_QUERY_INFORMATION, &request, sizeof(request), out, size,
                           &returned, nullptr) != 0;
}

// BatteryFlag bit 3. The SDK gives none of the BatteryFlag bits a name, so this
// is spelled out rather than borrowed from a header.
constexpr BYTE kChargingFlag = 0x08;

} // namespace

const wchar_t* charge_state_text(ChargeState charge) {
    switch (charge) {
    case ChargeState::Charging:
        return L"正在充电";
    case ChargeState::Full:
        return L"电池已充满";
    case ChargeState::PluggedIn:
        return L"已接通电源";
    default:
        return L"使用电池";
    }
}

BatteryState query_battery_state() {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        return {};
    }
    BatteryState state;
    // Like the original, treat "unknown" as full rather than showing nothing.
    state.percent = status.BatteryLifePercent == 255 ? 100 : static_cast<int>(status.BatteryLifePercent);
    // Charging comes from BatteryFlag, not from ACLineStatus: the line stays
    // online after the pack tops off, and this is the bit the shell's own
    // battery flyout reads, so the two cannot disagree on screen. Nothing
    // marks the two idle-on-AC states apart -- a pack stopped at a vendor
    // charge threshold looks exactly like a full one, save for the percent.
    if ((status.BatteryFlag & kChargingFlag) != 0) {
        state.charge = ChargeState::Charging;
    } else if (status.ACLineStatus == 1) {
        state.charge = state.percent >= 100 ? ChargeState::Full : ChargeState::PluggedIn;
    }
    return state;
}

bool BatteryDevice::bind(const wchar_t* path) {
    unique_file device(CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device) {
        return false;
    }

    // Every other battery IOCTL is keyed on this tag, which the driver changes
    // when the pack is swapped; an invalid tag is how it reports "no battery
    // here right now" without failing the call.
    ULONG tag = 0;
    ULONG wait = 0; // do not block waiting for a pack to appear
    DWORD returned = 0;
    if (!DeviceIoControl(device.get(), IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag),
                         &returned, nullptr) ||
        tag == BATTERY_TAG_INVALID) {
        return false;
    }

    BATTERY_INFORMATION information{};
    if (!query_information(device.get(), tag, BatteryInformation, &information,
                           static_cast<DWORD>(sizeof(information)))) {
        return false;
    }
    if ((information.Capabilities & BATTERY_SYSTEM_BATTERY) == 0 ||
        (information.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0) {
        return false;
    }

    facts_.design_mwh =
        information.DesignedCapacity == BATTERY_UNKNOWN_CAPACITY ? 0 : information.DesignedCapacity;
    facts_.full_charge_mwh =
        information.FullChargedCapacity == BATTERY_UNKNOWN_CAPACITY ? 0 : information.FullChargedCapacity;
    facts_.cycle_count = information.CycleCount;
    device_ = std::move(device);
    tag_ = tag;
    return true;
}

bool BatteryDevice::open() {
    close();

    // cfgmgr32 rather than setupapi: the same interface list costs one size call
    // and one fetch here, against a device info set and the two-call detail
    // struct there, and cfgmgr32.dll is just as much an inbox library.
    GUID interface_guid = kBatteryInterface;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, &interface_guid, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        length < 2) {
        return false;
    }

    std::wstring paths(length, L'\0');
    if (CM_Get_Device_Interface_ListW(&interface_guid, nullptr, paths.data(), length,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return false;
    }

    // A multi-sz of interface paths. Laptops report a single pack, so the first
    // device that answers wins; summing multiple packs would need their rates to
    // be in the same units, which the class driver does not promise.
    for (const wchar_t* path = paths.c_str(); *path != L'\0'; path += wcslen(path) + 1) {
        if (bind(path)) {
            return true;
        }
    }
    return false;
}

void BatteryDevice::close() noexcept {
    device_.reset();
    tag_ = 0;
    facts_ = {};
}

bool BatteryDevice::read_sample(BatterySample& sample) const {
    if (!device_) {
        return false;
    }

    BATTERY_WAIT_STATUS wait_status{};
    wait_status.BatteryTag = tag_;
    BATTERY_STATUS status{};
    DWORD returned = 0;
    if (!DeviceIoControl(device_.get(), IOCTL_BATTERY_QUERY_STATUS, &wait_status, sizeof(wait_status),
                         &status, sizeof(status), &returned, nullptr)) {
        return false;
    }

    sample = {};
    sample.remaining_mwh = status.Capacity == BATTERY_UNKNOWN_CAPACITY ? 0 : status.Capacity;
    // BATTERY_UNKNOWN_RATE is spelled 0x80000000, which is unsigned; comparing a
    // LONG against it directly is a signed/unsigned mismatch under /W4.
    sample.rate_mw = status.Rate == static_cast<LONG>(BATTERY_UNKNOWN_RATE) ? 0 : status.Rate;

    // Unlike CycleCount, an unsupported temperature fails the call outright
    // (ERROR_INVALID_FUNCTION / ERROR_NOT_SUPPORTED) instead of answering zero,
    // so availability is the return value, never the value itself.
    ULONG tenths_kelvin = 0; // BatteryTemperature reports 0.1 K units
    if (query_information(device_.get(), tag_, BatteryTemperature, &tenths_kelvin,
                          static_cast<DWORD>(sizeof(tenths_kelvin)))) {
        const double celsius = tenths_kelvin / 10.0 - 273.15;
        // Drivers have been seen returning 0 K or 2731 (exactly 0.00 C) for a
        // field they never filled in. Outside this range the number is either
        // made up or the pack is in trouble, and neither belongs on screen.
        if (celsius >= 0.0 && celsius <= 80.0) {
            sample.celsius = celsius;
            sample.has_temperature = true;
        }
    }
    return true;
}

BatteryDate BatteryDevice::read_manufacture_date() const {
    BatteryDate date;
    BATTERY_MANUFACTURE_DATE raw{};
    if (device_ && query_information(device_.get(), tag_, BatteryManufactureDate, &raw,
                                     static_cast<DWORD>(sizeof(raw))) &&
        raw.Year != 0) {
        date.year = raw.Year;
        date.month = raw.Month;
    }
    return date;
}

} // namespace bt
