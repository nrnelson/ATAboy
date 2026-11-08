// Device discovery and OS hooks for Linux (sysfs).

#if defined(__linux__)

#include "internal.h"
#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define USB_VID "cafe"
#define USB_PID "4005"
#define API_INTERFACE "ATAboy API"

static void read_line(const char *path, char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        size_t l = strlen(out);
        while (l && (out[l - 1] == '\n' || out[l - 1] == ' ' || out[l - 1] == '\r')) out[--l] = '\0';
        size_t s = 0;
        while (out[s] == ' ') s++;
        if (s) memmove(out, out + s, strlen(out + s) + 1);
    }
    fclose(f);
}

static bool is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

// The USB device directory (the one with idVendor/idProduct) at or above p.
static bool usb_device_dir(const char *path, char *out, size_t n) {
    char p[PATH_MAX];
    if (!realpath(path, p)) return false;
    char a[PATH_MAX + 16], b[PATH_MAX + 16];
    while (strlen(p) > 1) {
        snprintf(a, sizeof(a), "%s/idVendor", p);
        snprintf(b, sizeof(b), "%s/idProduct", p);
        if (is_file(a) && is_file(b)) { snprintf(out, n, "%s", p); return true; }
        char *s = strrchr(p, '/');
        if (!s) break;
        if (s == p) p[1] = '\0'; else *s = '\0';
    }
    return false;
}

static bool is_ataboy_usb(const char *usb) {
    char p[PATH_MAX + 16], v[16], d[16];
    snprintf(p, sizeof(p), "%s/idVendor", usb); read_line(p, v, sizeof(v));
    snprintf(p, sizeof(p), "%s/idProduct", usb); read_line(p, d, sizeof(d));
    return strcasecmp(v, USB_VID) == 0 && strcasecmp(d, USB_PID) == 0;
}

typedef struct { char usb[512]; char tty[64]; bool used; } port_t;

static int api_ports(port_t *ports, int max) {
    glob_t g;
    int n = 0;
    memset(&g, 0, sizeof(g));
    if (glob("/sys/class/tty/ttyACM*", 0, NULL, &g) != 0) { globfree(&g); return 0; }
    for (size_t i = 0; i < g.gl_pathc && n < max; i++) {
        char itf[PATH_MAX], p[PATH_MAX + 32], name[64];
        snprintf(p, sizeof(p), "%s/device", g.gl_pathv[i]);
        if (!realpath(p, itf)) continue;
        snprintf(p, sizeof(p), "%s/interface", itf);
        read_line(p, name, sizeof(name));
        if (strcmp(name, API_INTERFACE) != 0) continue;
        if (!usb_device_dir(itf, ports[n].usb, sizeof(ports[n].usb)) || !is_ataboy_usb(ports[n].usb)) continue;
        const char *base = strrchr(g.gl_pathv[i], '/');
        snprintf(ports[n].tty, sizeof(ports[n].tty), "%s", base ? base + 1 : g.gl_pathv[i]);
        ports[n].used = false;
        n++;
    }
    globfree(&g);
    return n;
}

int ab_find_devices(ab_devinfo *out, int max) {
    port_t ports[16];
    int np = api_ports(ports, 16);
    int count = 0;
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob("/sys/block/sd*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char p[PATH_MAX + 64], vendor[32];
            const char *blk = g.gl_pathv[i];
            snprintf(p, sizeof(p), "%s/device/vendor", blk);
            read_line(p, vendor, sizeof(vendor));
            if (strcmp(vendor, "ATAboy") != 0) continue;
            ab_devinfo d;
            memset(&d, 0, sizeof(d));
            const char *name = strrchr(blk, '/') + 1;
            snprintf(d.block, sizeof(d.block), "/dev/%s", name);
            glob_t sg;
            memset(&sg, 0, sizeof(sg));
            snprintf(p, sizeof(p), "%s/device/scsi_generic/sg*", blk);
            if (glob(p, 0, NULL, &sg) == 0 && sg.gl_pathc) {
                const char *s = strrchr(sg.gl_pathv[0], '/');
                snprintf(d.sg, sizeof(d.sg), "/dev/%s", s ? s + 1 : sg.gl_pathv[0]);
            }
            globfree(&sg);
            snprintf(p, sizeof(p), "%s/device", blk);
            char usb[512] = "";
            if (usb_device_dir(p, usb, sizeof(usb))) {
                snprintf(d.usb, sizeof(d.usb), "%s", usb);
                char sp[PATH_MAX + 16];
                snprintf(sp, sizeof(sp), "%s/serial", usb);
                read_line(sp, d.serial, sizeof(d.serial));
                for (int k = 0; k < np; k++)
                    if (!ports[k].used && strcmp(ports[k].usb, usb) == 0) {
                        snprintf(d.tty, sizeof(d.tty), "/dev/%s", ports[k].tty);
                        ports[k].used = true;
                        break;
                    }
            }
            snprintf(p, sizeof(p), "%s/device/rev", blk);
            read_line(p, d.rev, sizeof(d.rev));
            char num[32];
            snprintf(p, sizeof(p), "%s/size", blk);
            read_line(p, num, sizeof(num));
            d.size_sectors = strtoull(num, NULL, 10);
            snprintf(p, sizeof(p), "%s/device/timeout", blk);
            read_line(p, num, sizeof(num));
            d.scsi_timeout_s = atoi(num);
            snprintf(p, sizeof(p), "%s/device", blk);
            char real[PATH_MAX];
            if (realpath(p, real)) snprintf(d.sysfs, sizeof(d.sysfs), "%s", real);
            if (count < max) out[count] = d;
            count++;
        }
        globfree(&g);
    }
    for (int k = 0; k < np; k++) {           // API port without a disk (usb-storage not bound)
        if (ports[k].used) continue;
        if (count < max) {
            ab_devinfo d;
            memset(&d, 0, sizeof(d));
            snprintf(d.tty, sizeof(d.tty), "/dev/%s", ports[k].tty);
            snprintf(d.usb, sizeof(d.usb), "%s", ports[k].usb);
            char sp[PATH_MAX + 16];
            snprintf(sp, sizeof(sp), "%s/serial", ports[k].usb);
            read_line(sp, d.serial, sizeof(d.serial));
            out[count] = d;
        }
        count++;
    }
    return count;
}

static uint64_t sys_size(const ab_devinfo *d) {
    char p[160], num[32];
    const char *name = strrchr(d->block, '/');
    snprintf(p, sizeof(p), "/sys/block/%s/size", name ? name + 1 : d->block);
    read_line(p, num, sizeof(num));
    return strtoull(num, NULL, 10);
}

static int sys_ro(const ab_devinfo *d) {
    char p[160], num[32];
    const char *name = strrchr(d->block, '/');
    snprintf(p, sizeof(p), "/sys/block/%s/ro", name ? name + 1 : d->block);
    read_line(p, num, sizeof(num));
    return num[0] ? atoi(num) : -1;
}

static void poke(const ab_devinfo *d) {
    char p[sizeof(d->sysfs) + 16];
    snprintf(p, sizeof(p), "%s/rescan", d->sysfs);
    FILE *f = fopen(p, "w");                 // needs root; otherwise the kernel's own
    if (!f) return;                          // media-event polling notices in ~1-2 s
    fputs("1", f);
    fclose(f);
}

void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms) {
    if (!d || !d->sysfs[0] || !d->block[0]) return;
    uint64_t t0 = ab_now_ms();
    for (;;) {
        poke(d);
        bool ok = sys_size(d) == sectors && (want_ro < 0 || !sectors || sys_ro(d) == want_ro);
        if (ok || ab_now_ms() - t0 >= wait_ms) return;
        ab_sleep_ms(250);
    }
}

int ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s) {
    if (!d || !d->sysfs[0]) return -1;
    char p[sizeof(d->sysfs) + 16], num[32];
    snprintf(p, sizeof(p), "%s/timeout", d->sysfs);
    read_line(p, num, sizeof(num));
    if (!num[0]) return -1;
    int cur = atoi(num);
    if (cur < need_s) {
        FILE *f = fopen(p, "w");
        if (!f) return -1;
        fprintf(f, "%d", need_s);
        if (fclose(f) != 0) return -1;
        cur = need_s;
    }
    d->scsi_timeout_s = cur;
    return cur;
}

#endif
