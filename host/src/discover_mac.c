// Device discovery for macOS.  Placeholder until the IOKit version: lists
// /dev/cu.usbmodem* ports can't tell the menu port from the API port, so
// nothing is found automatically; pass the API port with --dev.

#if defined(__APPLE__)

#include "internal.h"

int ab_find_devices(ab_devinfo *out, int max) {
    (void)out; (void)max;
    return 0;
}

void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms) {
    (void)d; (void)sectors; (void)want_ro; (void)wait_ms;
}

int ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s) {
    (void)d; (void)need_s;
    return -1;
}

#endif
