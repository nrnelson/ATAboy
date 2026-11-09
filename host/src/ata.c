// Raw ATA commands through SCSI ATA PASS-THROUGH (16), which the firmware
// implements (SAT, firmware/sat.c).  Sent with Linux SG_IO on the ATAboy's
// SCSI node, also when the API itself goes over the serial port.  The
// firmware applies write protect and arming to these as to API requests.
//
// Linux only: SG_IO needs root.  On Windows the same CDB would go through
// IOCTL_SCSI_PASS_THROUGH(_DIRECT) on \\.\PhysicalDriveN, which needs an
// elevated process; not implemented yet.

#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATA_MAX_DATA (1u << 20)      // larger than any transfer the firmware accepts

void ab_ata_cdb(const ab_ata_cmd *c, uint8_t cdb[16]) {
    unsigned proto, t_dir;
    size_t n;
    if (c->data_out) { proto = 5; t_dir = 0; n = c->data_out_len; }       // PIO data-out
    else if (c->data_in) { proto = 4; t_dir = 1; n = c->data_in; }       // PIO data-in
    else { proto = 3; t_dir = 0; n = 0; }                                // non-data
    uint64_t f = (uint64_t)c->feature, k = (uint64_t)c->count, l = (uint64_t)c->lba;
    cdb[0] = 0x85;
    cdb[1] = (uint8_t)((proto << 1) | (c->ext ? 1 : 0));
    // CK_COND, T_DIR, BYT_BLOK + T_LENGTH = sector count field
    cdb[2] = (uint8_t)((c->ck_cond ? 0x20 : 0) | (t_dir << 3) | (n ? (0x04 | 0x02) : 0));
    cdb[3] = (uint8_t)(f >> 8);
    cdb[4] = (uint8_t)f;
    cdb[5] = (uint8_t)(k >> 8);
    cdb[6] = (uint8_t)k;
    cdb[7] = (uint8_t)(l >> 24);
    cdb[8] = (uint8_t)l;
    cdb[9] = (uint8_t)(l >> 32);
    cdb[10] = (uint8_t)(l >> 8);
    cdb[11] = (uint8_t)(l >> 40);
    cdb[12] = (uint8_t)(l >> 16);
    cdb[13] = (uint8_t)c->device;
    cdb[14] = (uint8_t)c->command;
    cdb[15] = 0;
}

// Registers from a SAT ATA Status Return descriptor (descriptor-format sense).
cJSON *ab_parse_ata_return(const uint8_t *sense, size_t len) {
    if (len < 8 || ((sense[0] & 0x7F) != 0x72 && (sense[0] & 0x7F) != 0x73)) return NULL;
    size_t i = 8, end = 8 + (size_t)sense[7], lim = end < len ? end : len;
    while (i + 1 < lim) {
        uint8_t code = sense[i], ln = sense[i + 1];
        if (code == 0x09 && i + 14 <= len) {
            const uint8_t *d = sense + i;
            bool ext = d[2] & 1;
            char sb[64], eb[64];
            uint64_t lba = d[7] | ((uint64_t)d[9] << 8) | ((uint64_t)d[11] << 16);
            if (ext) lba |= ((uint64_t)d[6] << 24) | ((uint64_t)d[8] << 32) | ((uint64_t)d[10] << 40);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "status", d[13]);
            cJSON_AddStringToObject(o, "status_bits", ab_status_bits(d[13], sb));
            cJSON_AddNumberToObject(o, "error", d[3]);
            cJSON_AddStringToObject(o, "error_bits", ab_error_bits(d[3], eb));
            cJSON_AddNumberToObject(o, "count", d[5] | (ext ? d[4] << 8 : 0));
            cJSON_AddNumberToObject(o, "lba", (double)lba);
            cJSON_AddNumberToObject(o, "device", d[12]);
            return o;
        }
        i += 2 + (size_t)ln;
    }
    return NULL;
}

// Sense key as Python's parse_sense_key() reads it (0 if too short).
static void sense_key(const uint8_t *s, size_t n, uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    *key = *asc = *ascq = 0;
    if (n < 3) return;
    uint8_t code = s[0] & 0x7F;
    if (code == 0x72 || code == 0x73) {
        *key = s[1] & 0x0F;
        *asc = s[2];
        *ascq = n > 3 ? s[3] : 0;
    } else {
        *key = s[2] & 0x0F;
        *asc = n > 12 ? s[12] : 0;
        *ascq = n > 13 ? s[13] : 0;
    }
}

cJSON *ab_ata_sense_result(int64_t command, const uint8_t *sense, size_t n) {
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "command", (double)command);
    cJSON *ok = cJSON_AddBoolToObject(out, "ok", true);
    char *hex = malloc(2 * n + 1);
    if (hex) {
        for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", sense[i]);
        hex[2 * n] = '\0';
    }
    cJSON_AddStringToObject(out, "sense", hex ? hex : "");
    free(hex);
    uint8_t key, asc, ascq;
    sense_key(sense, n, &key, &asc, &ascq);
    cJSON_AddNumberToObject(out, "sense_key", key);
    cJSON *regs = ab_parse_ata_return(sense, n);
    if (!regs) {
        cJSON_SetBoolValue(ok, false);
        const char *b = key == 7 && asc == 0x27 && ascq == 0 ? "write_protected"
                      : key == 7 && asc == 0x20 && ascq == 2 ? "not_armed"
                      : key == 5 && asc == 0x24 && ascq == 0 ? "invalid_field" : "rejected";
        cJSON_AddStringToObject(out, "blocked", b);
        return out;
    }
    cJSON *x;
    while ((x = regs->child) != NULL) {
        cJSON_DetachItemViaPointer(regs, x);
        cJSON_AddItemToObject(out, x->string, x);
    }
    cJSON_Delete(regs);
    cJSON_SetBoolValue(ok, key == 1);           // RECOVERED ERROR = the CK_COND report of a success
    return out;
}

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ab_ata(ab_dev *dev, const ab_ata_cmd *c, cJSON **out, uint8_t **data, size_t *data_len) {
    *out = NULL;
    if (data) *data = NULL;
    if (data_len) *data_len = 0;
    if (c->data_in > ATA_MAX_DATA || (c->data_out && c->data_out_len > ATA_MAX_DATA))
        return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "ATA data transfers are limited to %u bytes", ATA_MAX_DATA));
    const char *path = NULL;
    const ab_devinfo *di = ab_dev_info(dev);
    if (di) path = di->sg[0] ? di->sg : di->block[0] ? di->block : NULL;
    else if (!strcmp(ab_transport_name(dev), "sgio")) path = dev->path;
    if (!path || !*path)
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "no SCSI device for this ATAboy (needed for ATA pass-through)"));
    int flags = strstr(path, "/sg") ? O_RDWR : (O_RDONLY | O_NONBLOCK);
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            return ab_dev_fail(dev, ab_fail(AB_ERR_PERM, "no access to %s (ATA pass-through needs root)", path));
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "cannot open %s: %s", path, strerror(errno)));
    }
    uint8_t cdb[16], sense[96];
    ab_ata_cdb(c, cdb);
    memset(sense, 0, sizeof(sense));
    uint8_t *buf = NULL;
    sg_io_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    if (c->data_out) {
        h.dxfer_direction = SG_DXFER_TO_DEV;
        h.dxfer_len = (unsigned)c->data_out_len;
        h.dxferp = (void *)c->data_out;
    } else if (c->data_in) {
        buf = calloc(1, c->data_in);
        if (!buf) { close(fd); return ab_dev_fail(dev, ab_fail(AB_ERR_NOMEM, "out of memory")); }
        h.dxfer_direction = SG_DXFER_FROM_DEV;
        h.dxfer_len = c->data_in;
        h.dxferp = buf;
    } else {
        h.dxfer_direction = SG_DXFER_NONE;
    }
    h.cmd_len = 16;
    h.cmdp = cdb;
    h.mx_sb_len = sizeof(sense);
    h.sbp = sense;
    h.timeout = c->timeout_ms ? c->timeout_ms : 60000;
    int rc = ioctl(fd, SG_IO, &h);
    int err = errno;
    close(fd);
    if (rc != 0) {
        free(buf);
        if (err == EPERM || err == EACCES)
            return ab_dev_fail(dev, ab_fail(AB_ERR_PERM, "SG_IO needs root (CAP_SYS_RAWIO) for vendor/pass-through commands"));
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "SG_IO ioctl failed: %s", strerror(err)));
    }
    size_t slen = h.sb_len_wr < sizeof(sense) ? h.sb_len_wr : sizeof(sense);
    if (h.host_status || ((h.driver_status & 0x0F) && !slen)) {
        free(buf);
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "transport error host_status=0x%x driver_status=0x%x",
                                        h.host_status, h.driver_status));
    }
    if ((h.status == 0x02 || h.masked_status == 0x01 || slen) && (h.status != 0 || slen)) {
        free(buf);           // the firmware sends data or the registers, never both
        *out = ab_ata_sense_result(c->command, sense, slen);
        return AB_OK;
    }
    if (h.status) {
        free(buf);
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "SCSI status 0x%02x", h.status));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "command", (double)c->command);
    cJSON_AddBoolToObject(o, "ok", true);
    *out = o;
    if (buf) {
        int got = (int)c->data_in - h.resid;
        size_t n = got > 0 ? (size_t)got : 0;
        if (data && n) { *data = buf; buf = NULL; }
        if (data_len) *data_len = data ? n : 0;
    }
    free(buf);
    return AB_OK;
}

#else

int ab_ata(ab_dev *dev, const ab_ata_cmd *c, cJSON **out, uint8_t **data, size_t *data_len) {
    (void)c;
    *out = NULL;
    if (data) *data = NULL;
    if (data_len) *data_len = 0;
#if defined(_WIN32)
    return ab_dev_fail(dev, ab_fail(AB_ERR_UNSUPPORTED, "ATA pass-through is not implemented on Windows yet "
                                    "(it would need IOCTL_SCSI_PASS_THROUGH from an elevated process)"));
#else
    return ab_dev_fail(dev, ab_fail(AB_ERR_UNSUPPORTED, "ATA pass-through needs Linux SG_IO; not available on this platform"));
#endif
}

#endif
