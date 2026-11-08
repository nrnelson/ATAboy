// MSC callbacks — called on core 0 from within tud_task().
// NEVER call tud_task() from here.  IDE wait loops use busy_wait only.

#include "tusb.h"
#include "class/msc/msc_device.h"
#include "drive.h"
#include "config.h"
#include "sense.h"
#include "api.h"
#include "sat.h"
#include "version.h"
#include "usbbuf.h"
#include "msc_rw.h"
#include "pico/time.h"
#include <string.h>

// ---------------------------------------------------------------------------
//  Class-buffer tag (see usbbuf.h)
// ---------------------------------------------------------------------------

static uint8_t *msc_buf = NULL;
static uint32_t poison_seq = 0;
static const char poison_magic[12] = "ATABOY-NODAT";

static volatile bool     msc_active = false;
static volatile uint32_t msc_seq = 0;

void usbbuf_note(uint8_t *buf) {
    if (buf) msc_buf = buf;
    msc_active = true;
    msc_seq++;
}

bool usbbuf_msc_active(uint32_t *seq) { *seq = msc_seq; return msc_active; }

void usbbuf_poison(void) {
    msc_active = false;
    if (!msc_buf) return;
    poison_seq++;
    memcpy(msc_buf, poison_magic, 12);
    memcpy(msc_buf + 12, &poison_seq, 4);
}

bool usbbuf_has_host_data(const uint8_t *buf) {
    if (!msc_buf || buf != msc_buf) return false;     // address unknown yet: fail safe
    return memcmp(buf, poison_magic, 12) != 0 || memcmp(buf + 12, &poison_seq, 4) != 0;
}

// The complete callbacks run after the CSW of every command, passed or failed.
void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16]) { (void)lun; (void)scsi_cmd; usbbuf_poison(); msc_rw_end(); }
void tud_msc_read10_complete_cb(uint8_t lun)  { (void)lun; usbbuf_poison(); msc_rw_end(); }
void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; usbbuf_poison(); msc_rw_end(); }

// TinyUSB passes the host's transfer length as 'bufsize' even when it is
// larger than the class buffer, so cap it before touching the buffer.
static uint32_t buf_cap(uint32_t bufsize) {
    return bufsize < CFG_TUD_MSC_EP_BUFSIZE ? bufsize : CFG_TUD_MSC_EP_BUFSIZE;
}

// ---------------------------------------------------------------------------
//  MSC Required Callbacks
// ---------------------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "ATAboy";
    const char pid[] = "Hard Drive";
    const char rev[] = FW_INQUIRY_REV;

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

// TEST UNIT READY is answered by TinyUSB itself (as are INQUIRY, READ
// CAPACITY, MODE SENSE(6) and REQUEST SENSE), so this is where a mount or
// unmount is announced: UNIT ATTENTION 28h/00h makes the host's disk driver
// re-read capacity and the write-protect bit.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (media_changed_waiting && is_mounted) {
        media_changed_waiting = false;
        sense_set(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    if (!is_mounted) media_changed_waiting = false;   // "medium not present" says it all
    return is_mounted;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
    (void)lun;
    *block_size = 512;
    uint64_t ts = is_mounted ? drive_total_sectors() : 0;
    *block_count = (ts > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)ts;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

static int32_t not_present(uint8_t lun) {
    // Staged through sense.c like every other failure: TinyUSB overwrites its
    // own copy with NOT READY / MEDIUM NOT PRESENT when a READ10/WRITE10
    // callback fails, and the host's REQUEST SENSE gets the staged sense.
    sense_set(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    msc_rw_end();
    return -1;
}

uint32_t msc_rw_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// ---------------------------------------------------------------------------
//  READ10 / WRITE10 (data path in msc_rw.c)
// ---------------------------------------------------------------------------

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    usbbuf_note((uint8_t *)buffer);
    if (!is_mounted) return not_present(lun);
    return msc_rw_read(lun, lba, offset, (uint8_t *)buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    usbbuf_note(buffer);
    if (!is_mounted) return not_present(lun);
    if (config.drive_write_protected) {
        sense_set(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        msc_rw_end();
        return -1;
    }
    return msc_rw_write(lun, lba, offset, buffer, bufsize);
}

// ---------------------------------------------------------------------------
//  SCSI — Mode Sense, vendor API, ATA pass-through
// ---------------------------------------------------------------------------

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    uint8_t opcode = scsi_cmd[0];
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t cap = buf_cap(bufsize);
    usbbuf_note(buf);

    // API and pass-through work with or without a mounted drive and must not
    // swallow the mount/unmount UNIT ATTENTION meant for the host's sd driver.
    if (opcode == API_OPCODE) return api_handle(lun, scsi_cmd, buf, cap);
    if (opcode == 0x85 || opcode == 0xA1) return sat_passthrough(lun, scsi_cmd, buf, cap, bufsize);

    // Unit Attention on first access after mount/unmount
    if (is_mounted && media_changed_waiting) {
        sense_set(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0);
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
        if (cap < hdr) return -1;
        memset(buffer, 0, cap);
        uint16_t pos = hdr;

        // Page 0x03: Format Device (SPT)
        if ((page == 0x03 || page == 0x3F) && (uint32_t)pos + 24 <= cap) {
            buf[pos] = 0x03; buf[pos + 1] = 0x16;
            buf[pos + 10] = (uint8_t)(config.spt >> 8);
            buf[pos + 11] = (uint8_t)(config.spt & 0xFF);
            pos += 24;
        }
        // Page 0x04: Rigid Disk Geometry (Cyl/Heads)
        if ((page == 0x04 || page == 0x3F) && (uint32_t)pos + 24 <= cap) {
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

    case 0x00: return 0;  // TEST UNIT READY
    case 0x1B: return 0;  // START STOP UNIT
    case 0x35: return 0;  // SYNCHRONIZE CACHE
    case 0x1E: return 0;  // PREVENT ALLOW MEDIUM REMOVAL

    default:
        sense_set(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
        return -1;
    }
}
