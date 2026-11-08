// MSC callbacks — called on core 0 from within tud_task().
// NEVER call tud_task() from here.  IDE wait loops use busy_wait only.

#include "tusb.h"
#include "class/msc/msc_device.h"
#include "ide.h"
#include "config.h"
#include <string.h>
#include "pico/time.h"

extern volatile bool is_mounted;
extern volatile bool media_changed_waiting;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static uint64_t total_sectors(void) {
    if (!is_mounted) return 0;
    if (config.use_lba_mode) return config.lba_sectors;
    return (uint64_t)config.cyls * config.heads * config.spt;
}

// ---------------------------------------------------------------------------
//  MSC Required Callbacks
// ---------------------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "ATAboy";
    const char pid[] = "Hard Drive";
    const char rev[] = "6f3R";   // recovery build

    memset(vendor_id, ' ', 8);
    memcpy(vendor_id, vid, strlen(vid) < 8 ? strlen(vid) : 8);
    memset(product_id, ' ', 16);
    memcpy(product_id, pid, strlen(pid) < 16 ? strlen(pid) : 16);
    memset(product_rev, ' ', 4);
    memcpy(product_rev, rev, strlen(rev) < 4 ? strlen(rev) : 4);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return !config.drive_write_protected;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return is_mounted;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
    (void)lun;
    *block_size = 512;
    uint64_t ts = total_sectors();
    *block_count = (ts > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)ts;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

// ---------------------------------------------------------------------------
//  Recovery additions: real error pass-through, error log, vendor commands
// ---------------------------------------------------------------------------

#define ERRLOG_SIZE 64
typedef struct {
    uint32_t    seq;
    uint32_t    t_ms;           // ms since boot
    ide_error_t e;
} errlog_entry_t;

static errlog_entry_t errlog[ERRLOG_SIZE];
static uint32_t errlog_seq = 0;            // total failures ever logged

static bool        sense_pending = false;  // our detailed sense waiting for REQUEST SENSE
static ide_error_t sense_err;

static void errlog_add(const ide_error_t *e) {
    errlog_entry_t *x = &errlog[errlog_seq % ERRLOG_SIZE];
    x->seq  = ++errlog_seq;
    x->t_ms = to_ms_since_boot(get_absolute_time());
    x->e    = *e;
}

// Map ATA status/error to SCSI sense key / ASC / ASCQ (all MEDIUM ERROR or
// HARDWARE ERROR so the Linux SCSI layer does not silently retry).
static void ata_to_sense(const ide_error_t *e, uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    *key = SCSI_SENSE_MEDIUM_ERROR; *asc = 0x11; *ascq = 0x00;   // unrecovered read error
    if (e->timed_out)        { *asc = 0x3E; *ascq = 0x02; }        // timeout on logical unit
    else if (e->error & 0x40) { *asc = 0x11; *ascq = 0x00; }       // UNC
    else if (e->error & 0x10) { *asc = 0x14; *ascq = 0x01; }       // IDNF -> record not found
    else if (e->error & 0x01) { *asc = 0x13; *ascq = 0x00; }       // AMNF -> address mark not found
    else if (e->error & 0x80) { *asc = 0x11; *ascq = 0x00; }       // BBK  -> bad block mark
    else if (e->error & 0x04) { *key = SCSI_SENSE_HARDWARE_ERROR; *asc = 0x44; *ascq = 0x00; } // ABRT
}

static int32_t read_fail(uint8_t lun) {
    errlog_add(&ide_last_error);
    sense_err = ide_last_error;
    sense_pending = true;
    uint8_t key, asc, ascq;
    ata_to_sense(&sense_err, &key, &asc, &ascq);
    tud_msc_set_sense(lun, key, asc, ascq);   // TinyUSB overrides this; request_sense_cb restores it
    return -1;
}

// Fixed-format sense with the failing LBA in INFORMATION and the raw ATA
// registers in COMMAND-SPECIFIC INFORMATION:
//   bytes 3-6  : failing LBA (VALID bit set)
//   byte  8    : ATA status    byte 9 : ATA error
//   bytes 10-11: elapsed ms (capped 65535)
//   byte  14   : ATA command   byte 15: bit0 = timed out
int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize) {
    (void)lun;
    if (!sense_pending || bufsize < 18) return 18;
    uint8_t *b = (uint8_t *)buffer;
    uint8_t key, asc, ascq;
    ata_to_sense(&sense_err, &key, &asc, &ascq);
    uint32_t ms = sense_err.elapsed_ms > 0xFFFF ? 0xFFFF : sense_err.elapsed_ms;
    memset(b, 0, 18);
    b[0]  = 0xF0;                         // VALID | current, fixed format
    b[2]  = key;
    b[3]  = (uint8_t)(sense_err.lba >> 24);
    b[4]  = (uint8_t)(sense_err.lba >> 16);
    b[5]  = (uint8_t)(sense_err.lba >> 8);
    b[6]  = (uint8_t)(sense_err.lba);
    b[7]  = 10;                           // additional sense length
    b[8]  = sense_err.status;
    b[9]  = sense_err.error;
    b[10] = (uint8_t)(ms >> 8);
    b[11] = (uint8_t)ms;
    b[12] = asc;
    b[13] = ascq;
    b[14] = sense_err.cmd;
    b[15] = sense_err.timed_out ? 1 : 0;
    sense_pending = false;
    return 18;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Vendor command 0xC0 (10-byte CDB), sub-op in byte 1:
//   0x01 TIMEOUT   CDB[2..5]=new ms (0 = query).  Returns 4 bytes: current ms.
//   0x02 GET LOG   CDB[2] bit0 = clear after read.  Returns 8-byte header
//                  (total failures, entries returned) + 24-byte entries, oldest first.
//   0x03 READ LONG CDB[2..5]=LBA, CDB[6]=ECC bytes (default 4), CDB[7] bit0 = no retries.
//                  Returns 16-byte header + 512 data + ECC bytes.
//   0x04 IDENTIFY  Returns 512 bytes of IDENTIFY DEVICE data.
static int32_t vendor_cmd(uint8_t lun, uint8_t const cdb[16], uint8_t *buf, uint16_t bufsize) {
    switch (cdb[1]) {
    case 0x01: {
        uint32_t ms = get_be32(&cdb[2]);
        if (ms) {
            if (ms < 50) ms = 50;
            if (ms > 600000) ms = 600000;
            ide_read_timeout_ms = ms;
        }
        if (bufsize < 4) return -1;
        put_be32(buf, ide_read_timeout_ms);
        return 4;
    }
    case 0x02: {
        uint32_t n = errlog_seq < ERRLOG_SIZE ? errlog_seq : ERRLOG_SIZE;
        uint32_t fit = bufsize >= 8 ? (bufsize - 8) / 24 : 0;
        if (bufsize < 8) return -1;
        uint32_t ret = n < fit ? n : fit;
        put_be32(buf, errlog_seq);
        put_be32(buf + 4, ret);
        uint8_t *p = buf + 8;
        for (uint32_t i = 0; i < ret; i++) {
            const errlog_entry_t *x = &errlog[(errlog_seq - n + i) % ERRLOG_SIZE];
            put_be32(p, x->seq);
            put_be32(p + 4, x->t_ms);
            put_be32(p + 8, x->e.lba);
            p[12] = x->e.cmd; p[13] = x->e.status; p[14] = x->e.error; p[15] = x->e.timed_out;
            put_be32(p + 16, x->e.elapsed_ms);
            p[20] = x->e.tf_sn; p[21] = x->e.tf_cl; p[22] = x->e.tf_ch; p[23] = x->e.tf_dh;
            p += 24;
        }
        if (cdb[2] & 1) errlog_seq = 0;
        return (int32_t)(8 + ret * 24);
    }
    case 0x03: {
        if (!is_mounted) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0); return -1; }
        uint32_t lba = get_be32(&cdb[2]);
        uint8_t ecc = cdb[6] ? cdb[6] : 4;
        bool no_retry = cdb[7] & 1;
        if (lba >= total_sectors() || bufsize < 16 + 512 + ecc) {
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0);
            return -1;
        }
        memset(buf, 0, 16 + 512 + ecc);
        uint8_t st = 0;
        int32_t r = ide_read_long(lba, !no_retry, ecc, buf + 16, &st);
        if (r < 0) errlog_add(&ide_last_error);
        buf[0] = 0xA5;                                  // magic
        buf[1] = (r == 0) ? 1 : 0;                       // 1 = data transferred
        buf[2] = st;                                     // final status
        buf[3] = ide_last_error.error;
        buf[4] = ecc;
        buf[5] = ide_last_error.timed_out;
        put_be32(buf + 6, ide_last_error.elapsed_ms);
        put_be32(buf + 10, lba);
        return 16 + 512 + ecc;
    }
    case 0x04: {
        if (bufsize < 512) return -1;
        if (!ide_identify((uint16_t *)buf)) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0);
            return -1;
        }
        return 512;
    }
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0);
        return -1;
    }
}

// ---------------------------------------------------------------------------
//  READ10 — block transfer with partial first/last sector handling
// ---------------------------------------------------------------------------

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!is_mounted) return -1;

    uint64_t max = total_sectors();
    if (max == 0) return -1;

    uint32_t remaining = bufsize;
    uint8_t *ptr = (uint8_t *)buffer;
    uint32_t cur_lba = lba;

    // Partial first sector (non-zero offset)
    if (offset && remaining > 0 && cur_lba < max) {
        uint8_t temp[512];
        if (ide_read_sectors(cur_lba, 1, temp) < 0) {
            return read_fail(lun);
        }
        uint32_t n = 512 - offset;
        if (n > remaining) n = remaining;
        memcpy(ptr, temp + offset, n);
        ptr += n; remaining -= n; cur_lba++;
    }

    // Aligned middle sectors — single ATA command
    uint32_t aligned = remaining / 512;
    if (aligned > 0 && cur_lba < max) {
        if (cur_lba + aligned > max) aligned = (uint32_t)(max - cur_lba);
        if (ide_read_sectors(cur_lba, aligned, ptr) < 0) {
            return read_fail(lun);
        }
        ptr += aligned * 512; remaining -= aligned * 512; cur_lba += aligned;
    }

    // Partial last sector
    if (remaining > 0 && cur_lba < max) {
        uint8_t temp[512];
        if (ide_read_sectors(cur_lba, 1, temp) < 0) {
            return read_fail(lun);
        }
        memcpy(ptr, temp, remaining);
        ptr += remaining; remaining = 0;
    }

    if (remaining > 0) memset(ptr, 0, remaining);
    return (int32_t)bufsize;
}

// ---------------------------------------------------------------------------
//  WRITE10 — block transfer with partial first/last read-modify-write
// ---------------------------------------------------------------------------

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (!is_mounted || config.drive_write_protected) return -1;

    uint64_t max = total_sectors();
    if (max == 0) return -1;

    uint32_t remaining = bufsize;
    uint8_t *ptr = buffer;
    uint32_t cur_lba = lba;

    // Partial first sector — read-modify-write
    if (offset && remaining > 0 && cur_lba < max) {
        uint8_t temp[512];
        if (ide_read_sectors(cur_lba, 1, temp) < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
            return -1;
        }
        uint32_t n = 512 - offset;
        if (n > remaining) n = remaining;
        memcpy(temp + offset, ptr, n);
        if (ide_write_sectors(cur_lba, 1, temp) < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
            return -1;
        }
        ptr += n; remaining -= n; cur_lba++;
    }

    // Aligned middle sectors — single ATA command
    uint32_t aligned = remaining / 512;
    if (aligned > 0 && cur_lba < max) {
        if (cur_lba + aligned > max) aligned = (uint32_t)(max - cur_lba);
        if (ide_write_sectors(cur_lba, aligned, ptr) < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
            return -1;
        }
        ptr += aligned * 512; remaining -= aligned * 512; cur_lba += aligned;
    }

    // Partial last sector — read-modify-write
    if (remaining > 0 && cur_lba < max) {
        uint8_t temp[512];
        if (ide_read_sectors(cur_lba, 1, temp) < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
            return -1;
        }
        memcpy(temp, ptr, remaining);
        if (ide_write_sectors(cur_lba, 1, temp) < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
            return -1;
        }
        remaining = 0;
    }

    return (int32_t)bufsize;
}

// ---------------------------------------------------------------------------
//  SCSI — Mode Sense + misc
// ---------------------------------------------------------------------------

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    uint8_t opcode = scsi_cmd[0];
    uint8_t *buf = (uint8_t *)buffer;

    // Unit Attention on first access after mount/unmount
    if (is_mounted && media_changed_waiting) {
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0);
        media_changed_waiting = false;
        return -1;
    }

    switch (opcode) {
    case 0x1A:  // MODE SENSE (6)
    case 0x5A:  // MODE SENSE (10)
    {
        uint8_t page = scsi_cmd[2] & 0x3F;
        bool is10 = (opcode == 0x5A);
        uint8_t hdr = is10 ? 8 : 4;
        if (bufsize < hdr) return -1;
        memset(buffer, 0, bufsize);
        uint16_t pos = hdr;

        // Page 0x03: Format Device (SPT)
        if ((page == 0x03 || page == 0x3F) && pos + 24 <= bufsize) {
            buf[pos] = 0x03; buf[pos + 1] = 0x16;
            buf[pos + 10] = (uint8_t)(config.spt >> 8);
            buf[pos + 11] = (uint8_t)(config.spt & 0xFF);
            pos += 24;
        }
        // Page 0x04: Rigid Disk Geometry (Cyl/Heads)
        if ((page == 0x04 || page == 0x3F) && pos + 24 <= bufsize) {
            buf[pos] = 0x04; buf[pos + 1] = 0x16;
            buf[pos + 2] = 0;
            buf[pos + 3] = (uint8_t)(config.cyls >> 8);
            buf[pos + 4] = (uint8_t)(config.cyls & 0xFF);
            buf[pos + 5] = config.heads;
            pos += 24;
        }

        if (!is10) {
            buf[0] = (uint8_t)(pos - 1);
            buf[2] = config.drive_write_protected ? 0x80 : 0x00;
        } else {
            uint16_t full = pos - 2;
            buf[0] = (uint8_t)(full >> 8);
            buf[1] = (uint8_t)(full & 0xFF);
            buf[3] = config.drive_write_protected ? 0x80 : 0x00;
        }
        return (int32_t)pos;
    }

    case 0xC0: return vendor_cmd(lun, scsi_cmd, buf, bufsize);  // recovery vendor commands
    case 0x00: return 0;  // TEST UNIT READY
    case 0x1B: return 0;  // START STOP UNIT
    case 0x35: return 0;  // SYNCHRONIZE CACHE
    case 0x1E: return 0;  // PREVENT ALLOW MEDIUM REMOVAL

    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
        return -1;
    }
}
