// Host-side unit tests for the USB-disk READ(10)/WRITE(10) data path
// (firmware/msc_rw.c) against a fake drive.  Each test drives the callbacks
// the way TinyUSB does: chunks of up to 4096 bytes, called again for whatever
// a callback did not consume.
//   cc -Wall -Wextra -I firmware -o /tmp/t tests/test_rw.c firmware/msc_rw.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include "msc_rw.h"
#include "drive.h"
#include "sense.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define EP 4096u

// ---- fake platform -------------------------------------------------------

static uint64_t total = 1000;
static uint32_t now_ms = 0;
static uint32_t ms_per_sector = 0;      // simulated drive time per sector read/written
static unsigned char bad[1000];          // 1 = read fails at this LBA
static int read_cmds, write_cmds;
static uint32_t last_read_lba, last_read_count;
static errlog_entry_t last_fail;
static bool have_fail;
static uint8_t disk[1000][512];

static struct { int calls; uint8_t key, asc, ascq; bool media, budget; uint32_t info; } sense;
drive_stats_t drive_stats;

uint32_t msc_rw_now_ms(void) { return now_ms; }
uint64_t drive_total_sectors(void) { return total; }
const errlog_entry_t *drive_last_failure(void) { return have_fail ? &last_fail : NULL; }

static void fill(uint32_t lba, uint8_t *p) { memset(p, (int)(lba & 0xFF), 512); memcpy(p, &lba, 4); }

int drive_read(uint32_t lba, uint32_t count, uint8_t *buf, uint32_t *done) {
    read_cmds++;
    last_read_lba = lba; last_read_count = count;
    *done = 0;
    for (uint32_t i = 0; i < count; i++) {
        now_ms += ms_per_sector;
        if (bad[lba + i]) {
            memset(&last_fail, 0, sizeof(last_fail));
            last_fail.op = LOGOP_READ; last_fail.lba = lba; last_fail.count = (uint16_t)count;
            last_fail.done = (uint16_t)i; last_fail.fail_lba = lba + i; last_fail.result = IDE_ATA_ERROR;
            last_fail.error = 0x40;
            have_fail = true;
            return -1;
        }
        fill(lba + i, buf + i * 512);
        (*done)++;
    }
    return 0;
}

int drive_write(uint32_t lba, uint32_t count, const uint8_t *buf, uint32_t *done) {
    write_cmds++;
    for (uint32_t i = 0; i < count; i++) { now_ms += ms_per_sector; memcpy(disk[lba + i], buf + i * 512, 512); }
    *done = count;
    return 0;
}

void sense_set(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    (void)lun; sense.calls++; sense.key = key; sense.asc = asc; sense.ascq = ascq; sense.media = false; sense.info = 0;
}
void sense_set_media(uint8_t lun, const errlog_entry_t *e) {
    (void)lun; sense.calls++; sense.key = 3; sense.asc = 0x11; sense.ascq = 0; sense.media = true; sense.budget = false; sense.info = e->fail_lba;
}

void sense_set_budget(uint8_t lun, bool write, uint32_t lba, uint32_t elapsed_ms) {
    (void)lun; (void)elapsed_ms;
    sense.calls++; sense.key = 3; sense.asc = write ? 0x0C : 0x11; sense.ascq = write ? 0 : 0x11;
    sense.media = false; sense.budget = true; sense.info = lba;
}

static void reset(void) {
    memset(bad, 0, sizeof(bad));
    memset(&sense, 0, sizeof(sense));
    read_cmds = write_cmds = 0;
    have_fail = false;
    total = 1000;
    ms_per_sector = 0;
    memset(&drive_stats, 0, sizeof(drive_stats));
    now_ms += 100000;
    msc_rw_end();
}

// ---- TinyUSB model -------------------------------------------------------

// One READ(10) as TinyUSB runs it: returns bytes transferred before the CSW,
// -1 in *failed if the command failed.  'out' receives the data.
static uint32_t scsi_read(uint32_t lba, uint32_t total_bytes, uint32_t block_size, uint8_t *out, bool *failed) {
    uint32_t xferred = 0;
    static uint8_t buf[EP];
    *failed = false;
    while (xferred < total_bytes) {
        uint32_t n = total_bytes - xferred < EP ? total_bytes - xferred : EP;
        int32_t r = msc_rw_read(0, lba + xferred / block_size, xferred % block_size, buf, n);
        if (r < 0) { *failed = true; break; }
        CHECK(r > 0 && (uint32_t)r <= n);
        if (r <= 0) break;
        if (out) memcpy(out + xferred, buf, (size_t)r);
        xferred += (uint32_t)r;
    }
    msc_rw_end();                       // complete callback after the CSW
    return xferred;
}

static uint32_t scsi_write(uint32_t lba, uint32_t total_bytes, const uint8_t *data, bool *failed) {
    uint32_t xferred = 0;
    static uint8_t buf[EP];
    *failed = false;
    while (xferred < total_bytes) {
        uint32_t got = total_bytes - xferred < EP ? total_bytes - xferred : EP;
        memcpy(buf, data + xferred, got);
        // TinyUSB re-presents what a short return left over before receiving more
        while (got) {
            int32_t r = msc_rw_write(0, lba + xferred / 512, xferred % 512, buf, got);
            if (r < 0) { *failed = true; msc_rw_end(); return xferred + got; }
            memmove(buf, buf + r, got - (uint32_t)r);
            got -= (uint32_t)r; xferred += (uint32_t)r;
        }
    }
    msc_rw_end();
    return xferred;
}

// ---- tests -----------------------------------------------------------------

static uint8_t data[128 * 512];

static void test_clean_read(void) {
    reset();
    bool f;
    uint32_t n = scsi_read(10, 64 * 512, 512, data, &f);
    CHECK(!f && n == 64 * 512);
    CHECK(read_cmds == 8);
    uint32_t v; memcpy(&v, data + 63 * 512, 4);
    CHECK(v == 73);
}

static void test_malformed_cbw(void) {
    reset();
    uint8_t buf[EP];
    // one block of 4096 bytes: TinyUSB's block size 4096, second chunk offset 2048 etc.
    CHECK(msc_rw_read(0, 5, 512, buf, 512) == -1);
    CHECK(sense.key == 5 && sense.asc == 0x24);
    CHECK(read_cmds == 0);
    CHECK(msc_rw_read(0, 5, 0, buf, 600) == -1);
    CHECK(msc_rw_read(0, 5, 0, buf, 0) == -1);
    CHECK(msc_rw_write(0, 5, 100, buf, 512) == -1);
    CHECK(msc_rw_write(0, 5, 0, buf, 513) == -1);
    CHECK(read_cmds == 0 && write_cmds == 0);
    // block size 1000 (1 block, 1000 bytes): 1000 is not whole sectors
    bool f;
    CHECK(scsi_read(5, 1000, 1000, NULL, &f) == 0 && f);
}

static void test_bounds(void) {
    reset();
    bool f;
    uint8_t buf[EP];
    // entirely beyond the end
    CHECK(msc_rw_read(0, 1000, 0, buf, 512) == -1);
    CHECK(sense.key == 5 && sense.asc == 0x21 && read_cmds == 0);
    CHECK(msc_rw_read(0, 0xFFFFFFF0u, 0, buf, EP) == -1);
    // straddling: 996..999 real, then 5/21
    memset(data, 0xEE, sizeof(data));
    uint32_t n = scsi_read(996, 8 * 512, 512, data, &f);
    CHECK(f && n == 4 * 512);
    CHECK(sense.key == 5 && sense.asc == 0x21);
    CHECK(last_read_lba == 996 && last_read_count == 4);
    uint32_t v; memcpy(&v, data + 3 * 512, 4);
    CHECK(v == 999);
    CHECK(data[4 * 512] == 0xEE);      // nothing beyond: no zero fill
    // writes: in-range part written, rest refused
    reset();
    memset(disk, 0, sizeof(disk));
    memset(data, 0x5A, 8 * 512);
    n = scsi_write(998, 8 * 512, data, &f);
    CHECK(f && write_cmds == 1);
    CHECK(disk[999][0] == 0x5A && sense.key == 5 && sense.asc == 0x21);
    CHECK(msc_rw_write(0, 1000, 0, data, 512) == -1 && write_cmds == 1);
    // no geometry
    reset();
    total = 0;
    CHECK(msc_rw_read(0, 0, 0, buf, 512) == -1 && sense.key == 2 && sense.asc == 0x3A);
}

static void test_partial_read(void) {
    reset();
    bool f;
    bad[13] = 1;
    memset(data, 0xEE, sizeof(data));
    uint32_t n = scsi_read(10, 16 * 512, 512, data, &f);
    CHECK(f && n == 3 * 512);           // 10, 11, 12 delivered
    CHECK(read_cmds == 1);              // the bad sector was asked for once
    CHECK(sense.media && sense.info == 13);
    uint32_t v; memcpy(&v, data + 2 * 512, 4);
    CHECK(v == 12);
    CHECK(data[3 * 512] == 0xEE);

    // a later command reads the drive again (new attempt, new decision)
    read_cmds = 0;
    n = scsi_read(13, 512, 512, data, &f);
    CHECK(f && n == 0 && read_cmds == 1);

    // bad sector first in a chunk: fails at once, nothing delivered
    reset();
    bad[16] = 1;
    n = scsi_read(8, 16 * 512, 512, data, &f);
    CHECK(f && n == 8 * 512 && read_cmds == 2);
    CHECK(sense.info == 16);

    // a remembered failure is not used by a different command at the same LBA
    reset();
    bad[13] = 1;
    uint8_t buf[EP];
    CHECK(msc_rw_read(0, 10, 0, buf, EP) == 3 * 512);
    msc_rw_end();                       // command ended without reaching the bad LBA
    bad[13] = 0;                        // the drive would read it now
    read_cmds = 0;
    CHECK(msc_rw_read(0, 13, 0, buf, 512) == 512 && read_cmds == 1);

    // ... nor after a long gap (BOT / bus reset skipped the complete callback)
    reset();
    bad[13] = 1;
    CHECK(msc_rw_read(0, 10, 0, buf, EP) == 3 * 512);
    now_ms += MSC_RW_CONTINUE_MS + 1;
    bad[13] = 0;
    read_cmds = 0;
    CHECK(msc_rw_read(0, 13, 0, buf, 512) == 512 && read_cmds == 1);
}

static void test_budget(void) {
    reset();
    bool f;
    // 240 sectors (Linux's usual max) at 1 s a sector: 30 chunks of 8 s each.
    // The first chunk always runs; no chunk starts after 10 s.
    ms_per_sector = 1000;
    uint32_t t0 = now_ms;
    memset(data, 0xEE, sizeof(data));
    static uint8_t big[240 * 512];
    uint32_t n = scsi_read(100, 240 * 512, 512, big, &f);
    CHECK(f && n == 16 * 512);          // chunks at 0 and 8 s ran; the one at 16 s did not
    CHECK(read_cmds == 2);
    CHECK(sense.budget && sense.key == 3 && sense.asc == 0x11 && sense.ascq == 0x11 && sense.info == 116);
    CHECK(drive_stats.budget_stops == 1);
    CHECK(now_ms - t0 == 16000);
    uint32_t v; memcpy(&v, big + 15 * 512, 4);
    CHECK(v == 115);

    // the next command (the host reading the rest) gets a fresh budget
    read_cmds = 0;
    n = scsi_read(116, 16 * 512, 512, NULL, &f);
    CHECK(!f && n == 16 * 512 && read_cmds == 2);

    // a fast drive never meets the budget
    reset();
    ms_per_sector = 2;
    n = scsi_read(0, 240 * 512, 512, NULL, &f);
    CHECK(!f && n == 240 * 512 && drive_stats.budget_stops == 0);

    // a pending bad-sector failure is reported even after the budget ran out
    reset();
    bad[103] = 1;
    ms_per_sector = 6000;               // 3 good sectors + the bad one: 24 s in one chunk (> budget)
    uint8_t buf[EP];
    CHECK(msc_rw_read(0, 100, 0, buf, EP) == 3 * 512);
    CHECK(msc_rw_read(0, 103, 0, buf, EP) == -1);
    CHECK(sense.media && !sense.budget && sense.info == 103);
    CHECK(read_cmds == 1 && drive_stats.budget_stops == 0);
    msc_rw_end();

    // writes: same budget, WRITE ERROR sense
    reset();
    ms_per_sector = 1000;
    memset(big, 0x11, sizeof(big));
    n = scsi_write(0, 240 * 512, big, &f);
    CHECK(f && write_cmds == 2);
    CHECK(sense.budget && sense.asc == 0x0C && sense.info == 16);
}

int main(void) {
    test_clean_read();
    test_malformed_cbw();
    test_bounds();
    test_partial_read();
    test_budget();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all msc_rw tests passed\n");
    return 0;
}
