#include "battery_power.h"

#include "win32.h"
#include "win32_raii.h"

#include <winioctl.h> // CTL_CODE, which batclass.h builds its IOCTLs from

#include <batclass.h>
#include <cfgmgr32.h>

#include <cwchar>
#include <string>

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

bool read_device(const wchar_t* path, BatteryPower& power) {
    const unique_file device(CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
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
    // BATTERY_CAPACITY_RELATIVE means the capacity numbers are in units the
    // driver never defines, so mWh arithmetic on them is meaningless. Skipping
    // such a pack also skips UPS units, which are not the system battery.
    if ((information.Capabilities & BATTERY_SYSTEM_BATTERY) == 0 ||
        (information.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0) {
        return false;
    }

    BATTERY_WAIT_STATUS wait_status{};
    wait_status.BatteryTag = tag;
    BATTERY_STATUS status{};
    if (!DeviceIoControl(device.get(), IOCTL_BATTERY_QUERY_STATUS, &wait_status, sizeof(wait_status),
                         &status, sizeof(status), &returned, nullptr)) {
        return false;
    }

    power.full_charge_mwh =
        information.FullChargedCapacity == BATTERY_UNKNOWN_CAPACITY ? 0 : information.FullChargedCapacity;
    power.remaining_mwh = status.Capacity == BATTERY_UNKNOWN_CAPACITY ? 0 : status.Capacity;
    // BATTERY_UNKNOWN_RATE is spelled 0x80000000, which is unsigned; comparing a
    // LONG against it directly is a signed/unsigned mismatch under /W4.
    power.rate_mw = status.Rate == static_cast<LONG>(BATTERY_UNKNOWN_RATE) ? 0 : status.Rate;
    return true;
}

} // namespace

BatteryPower query_battery_power() {
    BatteryPower power;

    // cfgmgr32 rather than setupapi: the same interface list costs one size call
    // and one fetch here, against a device info set and the two-call detail
    // struct there, and cfgmgr32.dll is just as much an inbox library.
    GUID interface_guid = kBatteryInterface;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, &interface_guid, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        length < 2) {
        return power;
    }

    std::wstring paths(length, L'\0');
    if (CM_Get_Device_Interface_ListW(&interface_guid, nullptr, paths.data(), length,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return power;
    }

    // A multi-sz of interface paths. Laptops report a single pack, so the first
    // device that answers wins; summing multiple packs would need their rates to
    // be in the same units, which the class driver does not promise.
    for (const wchar_t* path = paths.c_str(); *path != L'\0'; path += wcslen(path) + 1) {
        if (read_device(path, power)) {
            break;
        }
    }
    return power;
}

} // namespace bt
