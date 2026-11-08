#include "sense.h"
#include "tusb.h"
#include <string.h>

#define SENSE_LEN 22     // > 18 bytes so Linux usb-storage switches to 96-byte auto-sense

static uint8_t pend[SENSE_LEN];
static bool    pend_valid = false;

void sense_set(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    memset(pend, 0, sizeof(pend));
    pend[0]  = 0x70;
    pend[2]  = key;
    pend[7]  = SENSE_LEN - 8;
    pend[12] = asc;
    pend[13] = ascq;
    pend_valid = true;
    tud_msc_set_sense(lun, key, asc, ascq);
}

// Map an ATA failure to SCSI sense for the USB disk.  The Linux SCSI layer
// (drivers/scsi/scsi_error.c) completes MEDIUM ERROR with ASC 11h/13h/14h
// without retrying, but retries every other MEDIUM ERROR, NOT READY and
// ABORTED COMMAND, and usb-storage sets retry_hwerror so HARDWARE ERROR is
// retried too.  A retry re-hammers a failing sector the drive has just spent
// seconds on, so every read failure is reported as MEDIUM ERROR 11h/13h/14h;
// the exact cause is in the vendor bytes (engine result, ATA registers).
static void map_ata(const errlog_entry_t *e, uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    *key = SCSI_SENSE_MEDIUM_ERROR; *asc = 0x11; *ascq = 0x00;     // unrecovered read error
    if (e->result == IDE_TIMEOUT)        { *ascq = 0x01; }          // read retries exhausted
    else if (e->error & 0x10) { *asc = 0x14; *ascq = 0x01; }       // IDNF -> record not found
    else if (e->error & 0x01) { *asc = 0x13; *ascq = 0x00; }       // AMNF
    if (e->op == LOGOP_WRITE && e->result == IDE_ATA_ERROR && !(e->error & 0x11)) {
        *asc = 0x0C; *ascq = 0x00;                                 // write error (retried by Linux)
    }
}

void sense_set_media(uint8_t lun, const errlog_entry_t *e) {
    uint8_t key, asc, ascq;
    map_ata(e, &key, &asc, &ascq);
    uint32_t ms = e->elapsed_ms > 0xFFFF ? 0xFFFF : e->elapsed_ms;
    memset(pend, 0, sizeof(pend));
    pend[0]  = 0xF0;                        // VALID | current, fixed format
    pend[2]  = key;
    pend[3]  = (uint8_t)(e->fail_lba >> 24);  // INFORMATION = failing LBA
    pend[4]  = (uint8_t)(e->fail_lba >> 16);
    pend[5]  = (uint8_t)(e->fail_lba >> 8);
    pend[6]  = (uint8_t)(e->fail_lba);
    pend[7]  = SENSE_LEN - 8;
    pend[8]  = e->status;                   // COMMAND-SPECIFIC INFORMATION
    pend[9]  = e->error;
    pend[10] = (uint8_t)(ms >> 8);
    pend[11] = (uint8_t)ms;
    pend[12] = asc;
    pend[13] = ascq;
    pend[14] = e->cmd;                      // FRU code = ATA command
    pend[15] = e->result == IDE_TIMEOUT ? 1 : 0;
    pend[18] = (uint8_t)e->result;          // ide result (signed)
    pend[19] = (e->tf_valid ? 1 : 0) | (e->recovered ? 2 : 0);
    pend[20] = (uint8_t)(e->done >> 8);
    pend[21] = (uint8_t)e->done;
    pend_valid = true;
    tud_msc_set_sense(lun, key, asc, ascq);
}

void sense_set_ata(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq, const ide_cmd_t *c) {
    memset(pend, 0, sizeof(pend));
    pend[0] = 0x72;                         // current, descriptor format
    pend[1] = key;
    pend[2] = asc;
    pend[3] = ascq;
    pend[7] = 14;                           // one 14-byte descriptor
    uint8_t *d = &pend[8];
    d[0]  = 0x09;                           // ATA Status Return
    d[1]  = 0x0C;
    d[2]  = c->ext ? 1 : 0;
    d[3]  = c->error;
    d[4]  = c->out_hob_count;
    d[5]  = c->out_count;
    d[6]  = c->out_hob_lba_lo;
    d[7]  = c->out_lba_lo;
    d[8]  = c->out_hob_lba_mid;
    d[9]  = c->out_lba_mid;
    d[10] = c->out_hob_lba_hi;
    d[11] = c->out_lba_hi;
    d[12] = c->out_device;
    d[13] = c->status;
    pend_valid = true;
    tud_msc_set_sense(lun, key, asc, ascq);
}

// TinyUSB has already built fixed-format sense from its own sense key; we
// either replace it with the staged sense or pad it to SENSE_LEN.
int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize) {
    (void)lun;
    uint8_t *b = (uint8_t *)buffer;
    if (bufsize < SENSE_LEN) return 18;
    if (pend_valid) {
        memcpy(b, pend, SENSE_LEN);
        pend_valid = false;
        return SENSE_LEN;
    }
    memset(b + 18, 0, SENSE_LEN - 18);
    b[7] = SENSE_LEN - 8;
    return SENSE_LEN;
}
