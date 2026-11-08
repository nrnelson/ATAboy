#include "sat.h"
#include "sense.h"
#include "drive.h"
#include "ata_policy.h"
#include "api.h"
#include "config.h"
#include "tusb.h"
#include <string.h>

// SAT protocol field values (CDB byte 1, bits 4:1)
#define SAT_HARD_RESET   0
#define SAT_SRST         1
#define SAT_NON_DATA     3
#define SAT_PIO_IN       4
#define SAT_PIO_OUT      5
#define SAT_DEV_DIAG     8
#define SAT_DEV_RESET    9
#define SAT_RETURN_INFO 15

#define ASC_PT_INFO 0x00
#define ASCQ_PT_INFO 0x1D    // ATA PASS-THROUGH INFORMATION AVAILABLE

static ide_cmd_t last_pt;    // for protocol 15 (return response information)

static int32_t reject(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    sense_set(lun, key, asc, ascq);
    return -1;
}

int32_t sat_passthrough(uint8_t lun, const uint8_t *cdb, uint8_t *buf, uint32_t cap, uint32_t xfer_len) {
    bool is16 = (cdb[0] == 0x85);
    uint8_t proto   = (cdb[1] >> 1) & 0x0F;
    bool    extend  = is16 && (cdb[1] & 0x01);
    bool    ck_cond = cdb[2] & 0x20;
    bool    t_dir_in = cdb[2] & 0x08;
    uint8_t t_length = cdb[2] & 0x03;

    ide_cmd_t c = {0};
    if (is16) {
        c.hob_feature = cdb[3];  c.feature = cdb[4];
        c.hob_count   = cdb[5];  c.count   = cdb[6];
        c.hob_lba_lo  = cdb[7];  c.lba_lo  = cdb[8];
        c.hob_lba_mid = cdb[9];  c.lba_mid = cdb[10];
        c.hob_lba_hi  = cdb[11]; c.lba_hi  = cdb[12];
        c.device = cdb[13];      c.command = cdb[14];
    } else {
        c.feature = cdb[3]; c.count = cdb[4];
        c.lba_lo = cdb[5];  c.lba_mid = cdb[6]; c.lba_hi = cdb[7];
        c.device = cdb[8];  c.command = cdb[9];
    }
    c.ext = extend;

    switch (proto) {
    case SAT_HARD_RESET:
        ide_reset_drive();
        return 0;
    case SAT_SRST:
    case SAT_DEV_RESET:
        ide_soft_reset();
        return 0;
    case SAT_RETURN_INFO:
        sense_set_ata(lun, SCSI_SENSE_RECOVERED_ERROR, ASC_PT_INFO, ASCQ_PT_INFO, &last_pt);
        return -1;
    case SAT_NON_DATA:
    case SAT_DEV_DIAG:
        c.proto = IDE_PROTO_NONDATA;
        break;
    case SAT_PIO_IN:
        c.proto = IDE_PROTO_PIO_IN;
        break;
    case SAT_PIO_OUT:
        c.proto = IDE_PROTO_PIO_OUT;
        break;
    default:                     // DMA, UDMA, FPDMA: the ATAboy only does PIO
        return reject(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
    }

    if (c.proto != IDE_PROTO_NONDATA) {
        // Data length comes from the host transfer; must be whole sectors and
        // fit the class buffer (CFG_TUD_MSC_EP_BUFSIZE).  T_DIR must agree with the protocol.
        if (t_length == 0 || xfer_len == 0 || (xfer_len % 512) || xfer_len > cap)
            return reject(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        if ((c.proto == IDE_PROTO_PIO_IN) != t_dir_in)
            return reject(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
        c.buf = buf;
        c.sectors = xfer_len / 512;
    } else if (xfer_len) {
        return reject(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
    }

    // Write-protect / arming policy
    ata_class_t cls = ata_classify(c.command, c.feature);
    if (c.proto == IDE_PROTO_PIO_OUT && cls == ATA_CLASS_READ) cls = ATA_CLASS_WRITE;
    // Any dangerous attempt uses up the arm, even one refused below.
    bool armed = (cls == ATA_CLASS_DANGEROUS) && api_take_arm();
    if (cls != ATA_CLASS_READ && config.drive_write_protected) {
        drive_stats.passthru_blocked++;
        return reject(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);   // write protected
    }
    if (cls == ATA_CLASS_DANGEROUS && !armed) {
        drive_stats.passthru_blocked++;
        return reject(lun, SCSI_SENSE_DATA_PROTECT, 0x20, 0x02);   // access denied: not armed
    }

    c.timeout_ms = c.proto == IDE_PROTO_NONDATA ? 30000
                 : (config.read_timeout_ms > 10000 ? config.read_timeout_ms : 10000);
    if (c.proto == IDE_PROTO_PIO_OUT && config.write_timeout_ms > c.timeout_ms)
        c.timeout_ms = config.write_timeout_ms;

    drive_stats.passthru_cmds++;
    int r = ide_exec(&c);
    if (r != IDE_OK) drive_stats.passthru_errors++;
    drive_record(LOGOP_PASSTHRU, &c, 0, c.sectors);
    last_pt = c;

    // Failures use ABORTED COMMAND: for SG_IO pass-through the Linux SCSI
    // layer never retries it, whereas HARDWARE ERROR is retried on USB
    // storage and would issue the ATA command a second time.
    if (r == IDE_ATA_ERROR) {
        sense_set_ata(lun, SCSI_SENSE_ABORTED_COMMAND, ASC_PT_INFO, ASCQ_PT_INFO, &c);
        return -1;
    }
    if (r != IDE_OK) {
        sense_set_ata(lun, SCSI_SENSE_ABORTED_COMMAND, 0x3E, 0x02, &c);   // timeout / protocol / bus trouble
        return -1;
    }
    // CK_COND on a non-data command: report the result registers.  (With a
    // data-in transfer the data takes precedence; TinyUSB can't send both.)
    if (ck_cond && c.proto == IDE_PROTO_NONDATA) {
        sense_set_ata(lun, SCSI_SENSE_RECOVERED_ERROR, ASC_PT_INFO, ASCQ_PT_INFO, &c);
        return -1;
    }
    return c.proto == IDE_PROTO_PIO_IN ? (int32_t)xfer_len : (c.proto == IDE_PROTO_PIO_OUT ? (int32_t)xfer_len : 0);
}
