// Device discovery for macOS (IOKit).  Finds the ATAboy's API serial port: an
// IOSerialBSDClient below the USB interface named "ATAboy API" (the interface
// string; or the CDC data interface right after it) on a USB device with
// VID 0xCAFE / PID 0x4005.  The port is its
// callout device (/dev/cu.*), the serial number the USB device's.  The USB
// disk (the whole-disk IOMedia under the same USB device) is reported for
// display; raw disk access needs root, so everything goes over the API port.

#if defined(__APPLE__)

#include "internal.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

#define ATABOY_VID 0xCAFE
#define ATABOY_PID 0x4005
#define MAX_DEPTH  16          // registry levels from the serial client up to the USB device
#define MAX_NODES  4096        // registry entries searched below the USB device for its disk

// A property of e (caller releases), or NULL.
static CFTypeRef get_prop(io_registry_entry_t e, const char *key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!k) return NULL;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    return v;
}

static bool prop_str(io_registry_entry_t e, const char *key, char *out, size_t n) {
    bool ok = false;
    CFTypeRef v = get_prop(e, key);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    if (!ok && n) out[0] = '\0';
    return ok;
}

static bool prop_i64(io_registry_entry_t e, const char *key, int64_t *out) {
    bool ok = false;
    CFTypeRef v = get_prop(e, key);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out);
    if (v) CFRelease(v);
    return ok;
}

static bool prop_true(io_registry_entry_t e, const char *key) {
    bool t = false;
    CFTypeRef v = get_prop(e, key);
    if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) t = CFBooleanGetValue((CFBooleanRef)v);
    if (v) CFRelease(v);
    return t;
}

static bool is_usb_interface(io_registry_entry_t e) {
    return IOObjectConformsTo(e, "IOUSBHostInterface") || IOObjectConformsTo(e, "IOUSBInterface");
}

static bool is_usb_device(io_registry_entry_t e) {
    return IOObjectConformsTo(e, "IOUSBHostDevice") || IOObjectConformsTo(e, "IOUSBDevice");
}

// Walk up from the serial client to its USB interface and USB device.  On
// success both come back retained (IOObjectRelease them).
static bool usb_parents(io_registry_entry_t svc, io_registry_entry_t *intf, io_registry_entry_t *dev) {
    *intf = *dev = IO_OBJECT_NULL;
    io_registry_entry_t cur = svc;
    IOObjectRetain(cur);
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent);
        IOObjectRelease(cur);
        cur = IO_OBJECT_NULL;
        if (kr != KERN_SUCCESS || parent == IO_OBJECT_NULL) break;
        cur = parent;
        if (*intf == IO_OBJECT_NULL && is_usb_interface(cur)) {
            IOObjectRetain(cur);
            *intf = cur;
        } else if (*intf != IO_OBJECT_NULL && is_usb_device(cur)) {
            IOObjectRetain(cur);
            *dev = cur;
            break;
        }
    }
    if (cur != IO_OBJECT_NULL) IOObjectRelease(cur);
    if (*intf != IO_OBJECT_NULL && *dev != IO_OBJECT_NULL) return true;
    if (*intf != IO_OBJECT_NULL) IOObjectRelease(*intf);
    if (*dev != IO_OBJECT_NULL) IOObjectRelease(*dev);
    *intf = *dev = IO_OBJECT_NULL;
    return false;
}

// Is the interface of usbdev with bInterfaceNumber num named want?
static bool interface_named(io_registry_entry_t usbdev, int64_t num, const char *want) {
    io_iterator_t it = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(usbdev, kIOServicePlane, &it) != KERN_SUCCESS) return false;
    bool found = false;
    io_registry_entry_t e;
    while (!found && (e = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        int64_t n = -1;
        char name[64];
        if (is_usb_interface(e) && prop_i64(e, "bInterfaceNumber", &n) && n == num &&
            prop_str(e, "USB Interface Name", name, sizeof(name)))
            found = strcmp(name, want) == 0;
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

// The interface string is on the CDC control interface; the serial client
// may hang off it or off the data interface that follows it (TinyUSB gives
// the data interface no string).
static bool is_api_port(io_registry_entry_t intf, io_registry_entry_t usbdev) {
    char name[64];
    if (prop_str(intf, "USB Interface Name", name, sizeof(name)) && name[0]) return strcmp(name, "ATAboy API") == 0;
    int64_t num = -1;
    return prop_i64(intf, "bInterfaceNumber", &num) && num >= 1 && interface_named(usbdev, num - 1, "ATAboy API");
}

// The whole-disk IOMedia below the USB device: BSD name and size.
static void find_disk(io_registry_entry_t usbdev, ab_devinfo *d) {
    io_iterator_t it = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(usbdev, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS)
        return;
    io_registry_entry_t e;
    int seen = 0;
    while ((e = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        bool done = false;
        if (IOObjectConformsTo(e, "IOMedia") && prop_true(e, "Whole")) {
            char bsd[48];
            int64_t size = 0;
            if (prop_str(e, "BSD Name", bsd, sizeof(bsd)) && bsd[0]) {
                snprintf(d->block, sizeof(d->block), "/dev/%s", bsd);
                if (prop_i64(e, "Size", &size) && size > 0) d->size_sectors = (uint64_t)size / 512;
                done = true;
            }
        }
        IOObjectRelease(e);
        if (done || ++seen >= MAX_NODES) break;
    }
    IOObjectRelease(it);
}

int ab_find_devices(ab_devinfo *out, int max) {
    CFMutableDictionaryRef match = IOServiceMatching("IOSerialBSDClient");
    if (!match) return 0;
    io_iterator_t it = IO_OBJECT_NULL;
    // Consumes 'match'.  MACH_PORT_NULL selects the default main port on
    // every macOS version (kIOMainPortDefault needs 12+).
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS) return 0;
    int n = 0;
    io_registry_entry_t svc;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        io_registry_entry_t intf, usbdev;
        char tty[256];
        if (prop_str(svc, "IOCalloutDevice", tty, sizeof(tty)) && tty[0] && usb_parents(svc, &intf, &usbdev)) {
            int64_t vid = -1, pid = -1;
            if (prop_i64(usbdev, "idVendor", &vid) && vid == ATABOY_VID &&
                prop_i64(usbdev, "idProduct", &pid) && pid == ATABOY_PID && is_api_port(intf, usbdev)) {
                if (out && n < max) {
                    ab_devinfo *d = &out[n];
                    memset(d, 0, sizeof(*d));
                    snprintf(d->tty, sizeof(d->tty), "%s", tty);
                    prop_str(usbdev, "USB Serial Number", d->serial, sizeof(d->serial));
                    find_disk(usbdev, d);
                }
                n++;
            }
            IOObjectRelease(intf);
            IOObjectRelease(usbdev);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return n;
}

void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms) {
    (void)d; (void)sectors; (void)want_ro; (void)wait_ms;
}

int ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s) {
    (void)d; (void)need_s;
    return -1;
}

#endif
