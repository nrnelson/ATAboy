// READ(10) / WRITE(10) data path of the USB disk (see msc_rw.h).

#include "msc_rw.h"
#include "drive.h"
#include "sense.h"
#include <string.h>

// SCSI sense keys (tusb.h is not included so the module builds on the host)
#define SK_NOT_READY        0x02
#define SK_MEDIUM_ERROR     0x03
#define SK_ILLEGAL_REQUEST  0x05

static struct {
    bool     active;          // a READ(10)/WRITE(10) is in progress
    bool     write;
    uint32_t next_lba;        // where its next chunk must start
    uint32_t last_ms;         // when the previous chunk returned
    bool     fail_pending;    // the drive failed at next_lba: fail the next chunk with 'fail'
    errlog_entry_t fail;      // copy: the error log may move on before the next chunk
} cmd;

void msc_rw_end(void) {
    cmd.active = false;
    cmd.fail_pending = false;
}

// Called at the start of every chunk.  A chunk continues the command in
// progress only if no complete callback came in between, it has the same
// direction, starts where the previous chunk ended and follows it promptly.
static void enter(bool write, uint32_t lba) {
    uint32_t now = msc_rw_now_ms();
    if (cmd.active && cmd.write == write && cmd.next_lba == lba &&
        now - cmd.last_ms <= MSC_RW_CONTINUE_MS) return;
    msc_rw_end();
    cmd.active = true;
    cmd.write = write;
}

static void advance(uint32_t lba) {
    cmd.next_lba = lba;
    cmd.last_ms = msc_rw_now_ms();
}

static int32_t fail(void) {
    msc_rw_end();
    return -1;
}

static int32_t media_fail(uint8_t lun, const errlog_entry_t *e) {
    if (e) sense_set_media(lun, e);
    else   sense_set(lun, SK_MEDIUM_ERROR, 0x11, 0x00);
    return fail();
}

// TinyUSB derives the block size from the host's transfer length
// (total_bytes / block_count) without checking it against READ CAPACITY, so a
// malformed CBW gives offsets inside a "block" and lengths that are not whole
// sectors.  Only whole 512-byte sectors starting on a sector boundary are
// served; anything else is ILLEGAL REQUEST / INVALID FIELD IN CDB.
static bool whole_sectors(uint8_t lun, uint32_t offset, uint32_t bufsize) {
    if (offset == 0 && bufsize > 0 && bufsize % 512 == 0) return true;
    sense_set(lun, SK_ILLEGAL_REQUEST, 0x24, 0x00);
    return false;
}

// Sectors of this chunk that exist; 0 (with sense staged) if none.  Only real
// sectors are transferred: a chunk that runs past the end returns the sectors
// before it, and TinyUSB's call for the rest lands at the end and fails with
// ILLEGAL REQUEST / LOGICAL BLOCK ADDRESS OUT OF RANGE.
static uint32_t in_range(uint8_t lun, uint32_t lba, uint32_t bufsize) {
    uint64_t max = drive_total_sectors();
    if (max == 0) { sense_set(lun, SK_NOT_READY, 0x3A, 0x00); return 0; }
    if (lba >= max) { sense_set(lun, SK_ILLEGAL_REQUEST, 0x21, 0x00); return 0; }
    uint32_t n = bufsize / 512;
    if ((uint64_t)lba + n > max) n = (uint32_t)(max - lba);
    return n;
}

int32_t msc_rw_read(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buf, uint32_t bufsize) {
    if (!whole_sectors(lun, offset, bufsize)) return fail();
    enter(false, lba);

    // The previous chunk ended at a sector the drive failed on: report that
    // failure now.  Asking the drive again would cost a second attempt (and
    // seconds of retries) at a sector it has just given up on.
    if (cmd.fail_pending) {
        sense_set_media(lun, &cmd.fail);
        return fail();
    }

    uint32_t n = in_range(lun, lba, bufsize);
    if (n == 0) return fail();

    uint32_t done = 0;
    if (drive_read(lba, n, buf, &done) == 0) {
        advance(lba + n);
        return (int32_t)(n * 512);
    }
    const errlog_entry_t *e = drive_last_failure();
    if (done == 0 || done >= n || !e) return media_fail(lun, e);

    // Hand over the good sectors; TinyUSB calls back at the bad one.
    cmd.fail = *e;
    cmd.fail_pending = true;
    advance(lba + done);
    return (int32_t)(done * 512);
}

int32_t msc_rw_write(uint8_t lun, uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t bufsize) {
    if (!whole_sectors(lun, offset, bufsize)) return fail();
    enter(true, lba);

    uint32_t n = in_range(lun, lba, bufsize);
    if (n == 0) return fail();

    uint32_t done = 0;
    if (drive_write(lba, n, buf, &done) != 0) return media_fail(lun, drive_last_failure());
    advance(lba + n);
    return (int32_t)(n * 512);    // short past the end: TinyUSB hands the rest back
}
