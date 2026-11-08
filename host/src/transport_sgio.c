// API requests as vendor SCSI commands through Linux SG_IO.  Vendor opcodes
// are outside the kernel's unprivileged whitelist, so this needs root
// (CAP_SYS_RAWIO); the serial transport doesn't.

#if defined(__linux__)

#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    ab_transport base;
    int fd;
} sgio_tp;

static int sg_api(ab_transport *base, const uint8_t cdb_in[16], uint32_t alloc, uint32_t timeout_ms,
                  uint8_t *out, size_t *out_len, bool *maybe_executed, uint8_t sense3[3]) {
    sgio_tp *t = (sgio_tp *)base;
    uint8_t cdb[16], sense[64];
    memcpy(cdb, cdb_in, 16);
    memset(sense, 0, sizeof(sense));
    sg_io_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.dxfer_direction = SG_DXFER_FROM_DEV;
    h.cmd_len = 16;
    h.mx_sb_len = sizeof(sense);
    h.dxfer_len = alloc;
    h.dxferp = out;
    h.cmdp = cdb;
    h.sbp = sense;
    h.timeout = timeout_ms;
    *out_len = 0;
    *maybe_executed = true;
    if (ioctl(t->fd, SG_IO, &h) != 0) {
        if (errno == EPERM || errno == EACCES)
            return ab_fail(AB_ERR_PERM, "SG_IO needs root (CAP_SYS_RAWIO) for vendor commands");
        return ab_fail(AB_ERR_TRANSPORT, "SG_IO failed: %s", strerror(errno));
    }
    if (h.host_status || ((h.driver_status & 0x0F) && !h.sb_len_wr))
        return ab_fail(AB_ERR_TRANSPORT, "transport error host_status=0x%x driver_status=0x%x",
                       h.host_status, h.driver_status);
    if (h.status == 0x02 || h.sb_len_wr) {
        uint8_t key = 0, asc = 0, ascq = 0;
        if (h.sb_len_wr >= 4 && (sense[0] & 0x7F) >= 0x72) { key = sense[1] & 0x0F; asc = sense[2]; ascq = sense[3]; }
        else if (h.sb_len_wr >= 14) { key = sense[2] & 0x0F; asc = sense[12]; ascq = sense[13]; }
        sense3[0] = key; sense3[1] = asc; sense3[2] = ascq;
        return ab_fail(AB_ERR_CHECK, "CHECK CONDITION sense %X/%02X/%02X", key, asc, ascq);
    }
    if (h.status) return ab_fail(AB_ERR_TRANSPORT, "SCSI status 0x%02x", h.status);
    int n = (int)alloc - h.resid;
    *out_len = n > 0 ? (size_t)n : 0;
    return AB_OK;
}

static void sg_close(ab_transport *base) {
    sgio_tp *t = (sgio_tp *)base;
    close(t->fd);
    free(t);
}

int ab_sgio_transport_open(const char *path, ab_transport **out) {
    int flags = strstr(path, "/sg") ? O_RDWR : (O_RDONLY | O_NONBLOCK);
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            return ab_fail(AB_ERR_PERM, "no access to %s (SG_IO needs root)", path);
        return ab_fail(AB_ERR_TRANSPORT, "cannot open %s: %s", path, strerror(errno));
    }
    sgio_tp *t = calloc(1, sizeof(*t));
    if (!t) { close(fd); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    t->fd = fd;
    t->base.name = "sgio";
    t->base.api = sg_api;
    t->base.close = sg_close;
    *out = &t->base;
    return AB_OK;
}

#endif
