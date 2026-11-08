// Device discovery for Windows (SetupAPI).  Finds the ATAboy's API serial
// port: a COM port of USB VID_CAFE&PID_4005 whose bus-reported name (the USB
// interface string) is "ATAboy API"; the USB serial number comes from the
// parent composite device's instance id.  The USB disk isn't used on
// Windows (raw disk access needs admin); everything goes over the API port.

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "internal.h"
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static bool icontains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && toupper((unsigned char)hay[i]) == toupper((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static bool is_api_interface(HDEVINFO h, SP_DEVINFO_DATA *d, const char *hwid) {
    WCHAR name[128];
    DEVPROPTYPE type = 0;
    if (SetupDiGetDevicePropertyW(h, d, &DEVPKEY_Device_BusReportedDeviceDesc, &type, (PBYTE)name,
                                  sizeof(name), NULL, 0) && type == DEVPROP_TYPE_STRING)
        return wcscmp(name, L"ATAboy API") == 0;
    return icontains(hwid, "&MI_03");          // older Windows: the API port is interface 3
}

int ab_find_devices(ab_devinfo *out, int max) {
    int count = 0;
    HDEVINFO h = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 0;
    for (DWORD i = 0;; i++) {
        SP_DEVINFO_DATA d;
        memset(&d, 0, sizeof(d));
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(h, i, &d)) break;
        char hwid[1024] = "";
        if (!SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_HARDWAREID, NULL, (PBYTE)hwid, sizeof(hwid) - 2, NULL))
            continue;
        if (!icontains(hwid, "VID_CAFE&PID_4005") || !is_api_interface(h, &d, hwid)) continue;
        char port[64] = "";
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            DWORD type = 0, len = sizeof(port) - 1;
            if (RegQueryValueExA(k, "PortName", NULL, &type, (LPBYTE)port, &len) != ERROR_SUCCESS || type != REG_SZ)
                port[0] = '\0';
            port[sizeof(port) - 1] = '\0';
            RegCloseKey(k);
        }
        if (!port[0]) continue;
        ab_devinfo info;
        memset(&info, 0, sizeof(info));
        snprintf(info.tty, sizeof(info.tty), "%s", port);
        DEVINST parent;
        char id[MAX_DEVICE_ID_LEN + 1];
        if (CM_Get_Parent(&parent, d.DevInst, 0) == CR_SUCCESS &&
            CM_Get_Device_IDA(parent, id, sizeof(id), 0) == CR_SUCCESS) {
            const char *s = strrchr(id, '\\');
            if (s && !strchr(s, '&')) snprintf(info.serial, sizeof(info.serial), "%s", s + 1);
        }
        if (count < max) out[count] = info;
        count++;
    }
    SetupDiDestroyDeviceInfoList(h);
    return count;
}

void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms) {
    (void)d; (void)sectors; (void)want_ro; (void)wait_ms;   // Windows notices media changes itself
}

int ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s) {
    (void)d; (void)need_s;
    return -1;
}

#endif
