#include "ide.h"
#include "ide_pio.h"
#include "config.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/sync.h"
#include <string.h>

static uint8_t dev_base = 0xA0;   // 0xA0 = master, 0xB0 = slave
static recursive_mutex_t ide_mtx;

void ide_lock(void)   { recursive_mutex_enter_blocking(&ide_mtx); }
void ide_unlock(void) { recursive_mutex_exit(&ide_mtx); }

void    ide_select_device(uint8_t base) { dev_base = (base == 0xB0) ? 0xB0 : 0xA0; }
uint8_t ide_selected_device(void)       { return dev_base; }

// ---------------------------------------------------------------------------
//  Bus helpers — address, transceiver, and chip-select (all SIO-managed)
//  Everything static in this file assumes the caller holds ide_mtx.
// ---------------------------------------------------------------------------

static void bus_idle(void) {
    sio_hw->gpio_set = (1 << IDE_CS0) | (1 << IDE_CS1);
    sio_hw->gpio_clr = (1 << IDE_DIR) | (1 << IDE_DIR1);   // DIR = read
    sio_hw->gpio_set = (1 << IDE_OE) | (1 << IDE_OE1);     // OE disabled
}

static void set_address(uint8_t addr) {
    sio_hw->gpio_clr = ADDR_MASK;
    sio_hw->gpio_set = (uint32_t)(addr & 0x07) << IDE_A0;
}

static void xcvr_read(void) {
    sio_hw->gpio_set = (1 << IDE_OE) | (1 << IDE_OE1);     // disable first
    sio_hw->gpio_clr = (1 << IDE_DIR) | (1 << IDE_DIR1);   // DIR = IDE -> Pico
    busy_wait_at_least_cycles(20);                            // 74HCT245 switch time
    sio_hw->gpio_clr = (1 << IDE_OE) | (1 << IDE_OE1);     // enable
}

static void xcvr_write(void) {
    sio_hw->gpio_set = (1 << IDE_OE) | (1 << IDE_OE1);     // disable first
    sio_hw->gpio_set = (1 << IDE_DIR) | (1 << IDE_DIR1);   // DIR = Pico -> IDE
    busy_wait_at_least_cycles(20);
    sio_hw->gpio_clr = (1 << IDE_OE) | (1 << IDE_OE1);     // enable
}

// ---------------------------------------------------------------------------
//  Register I/O — single 8-bit reads/writes via PIO strobes (unlocked)
// ---------------------------------------------------------------------------

static void wr(uint8_t reg, uint8_t val) {
    set_address(reg);
    xcvr_write();
    sio_hw->gpio_clr = (1 << IDE_CS0);
    uint16_t word = (uint16_t)val;
    ide_pio_write(1, &word);
    sio_hw->gpio_set = (1 << IDE_CS0);
    bus_idle();
}

static uint8_t rd(uint8_t reg) {
    set_address(reg);
    xcvr_read();
    sio_hw->gpio_clr = (1 << IDE_CS0);
    uint16_t val = 0xFFFF;
    ide_pio_read(1, &val);
    sio_hw->gpio_set = (1 << IDE_CS0);
    bus_idle();
    return (uint8_t)(val & 0xFF);
}

static uint8_t rd_alt(void) {
    set_address(6);
    xcvr_read();
    sio_hw->gpio_clr = (1 << IDE_CS1);
    uint16_t val = 0xFFFF;
    ide_pio_read(1, &val);
    sio_hw->gpio_set = (1 << IDE_CS1);
    bus_idle();
    return (uint8_t)(val & 0xFF);
}

static void wr_ctl(uint8_t val) {
    set_address(6);
    xcvr_write();
    sio_hw->gpio_clr = (1 << IDE_CS1);
    uint16_t word = (uint16_t)val;
    ide_pio_write(1, &word);
    sio_hw->gpio_set = (1 << IDE_CS1);
    bus_idle();
}

// 16-bit data register bursts
static bool data_in(uint16_t *buf, uint32_t words) {
    set_address(0);
    xcvr_read();
    sio_hw->gpio_clr = (1 << IDE_CS0);
    bool ok = ide_pio_read(words, buf);
    sio_hw->gpio_set = (1 << IDE_CS0);
    bus_idle();
    return ok;
}

static bool data_out(const uint16_t *buf, uint32_t words) {
    set_address(0);
    xcvr_write();
    sio_hw->gpio_clr = (1 << IDE_CS0);
    bool ok = ide_pio_write(words, buf);
    sio_hw->gpio_set = (1 << IDE_CS0);
    bus_idle();
    return ok;
}

// ---------------------------------------------------------------------------
//  Public locked register access
// ---------------------------------------------------------------------------

void ide_write_reg(uint8_t reg, uint8_t val) { ide_lock(); wr(reg, val); ide_unlock(); }
uint8_t ide_read_reg(uint8_t reg)  { ide_lock(); uint8_t v = rd(reg); ide_unlock(); return v; }
uint8_t ide_read_alt_status(void)  { ide_lock(); uint8_t v = rd_alt(); ide_unlock(); return v; }
void ide_write_control(uint8_t val) { ide_lock(); wr_ctl(val); ide_unlock(); }

void ide_set_iordy(bool enabled) {
    gpio_set_inover(IDE_IORDY, enabled ? GPIO_OVERRIDE_NORMAL : GPIO_OVERRIDE_HIGH);
}

// ---------------------------------------------------------------------------
//  Status polling (unlocked)
// ---------------------------------------------------------------------------

// Wait for BSY=0 (and DRDY=1 if need_drdy).  *st = last status read.
static bool wait_ready(uint32_t timeout_ms, bool need_drdy, uint8_t *st) {
    uint32_t t0 = time_us_32();
    uint32_t limit = timeout_ms * 1000u;
    uint8_t s;
    for (;;) {
        s = rd(7);
        if (!(s & ATA_ST_BSY) && (!need_drdy || (s & ATA_ST_DRDY))) { if (st) *st = s; return true; }
        if (time_us_32() - t0 >= limit) { if (st) *st = s; return false; }
        busy_wait_us_32(10);
    }
}

// Wait for DRQ (data ready) or ERR.  Status bits are only trusted with BSY=0.
// Returns 1 = DRQ, 0 = ERR/DF without data, -1 = timeout.
// take_err_data: ERR and DRQ both set -> report DRQ (caller reads the block).
static int wait_drq(uint32_t limit_us, bool take_err_data, uint8_t *st_out) {
    uint32_t t0 = time_us_32();
    uint8_t st;
    for (;;) {
        st = rd(7);   // reading STATUS also clears INTRQ
        if (!(st & ATA_ST_BSY)) {
            if (take_err_data && (st & ATA_ST_DRQ)) { *st_out = st; return 1; }
            if (st & (ATA_ST_ERR | ATA_ST_DF)) { *st_out = st; return 0; }
            if (st & ATA_ST_DRQ) { *st_out = st; return 1; }
        }
        if (time_us_32() - t0 >= limit_us) { *st_out = st; return -1; }
        busy_wait_us_32(10);
    }
}

// Wait for BSY=0 after a command or the last data block.  -1 = timeout.
static int wait_not_busy(uint32_t limit_us, uint8_t *st_out) {
    uint32_t t0 = time_us_32();
    uint8_t st;
    for (;;) {
        st = rd(7);
        if (!(st & ATA_ST_BSY)) { *st_out = st; return 0; }
        if (time_us_32() - t0 >= limit_us) { *st_out = st; return -1; }
        busy_wait_us_32(10);
    }
}

static void drain_sector(void) {
    uint16_t discard[256];
    data_in(discard, 256);
}

static bool set_geometry(uint8_t heads, uint8_t spt) {
    wr(6, dev_base | ((heads - 1) & 0x0F));
    wr(2, spt);
    wr(7, 0x91);                          // INITIALIZE DEVICE PARAMETERS
    uint8_t st;
    if (!wait_ready(1000, true, &st)) return false;
    return !(st & ATA_ST_ERR);
}

// SRST aborts whatever the drive is doing and clears INITIALIZE DEVICE
// PARAMETERS, so CHS geometry is re-applied afterwards.
static void soft_reset(void) {
    wr_ctl(0x04);
    busy_wait_us_32(10);
    wr_ctl(0x00);
    busy_wait_us_32(2000);
    wait_ready(2000, true, NULL);
    if (!config.use_lba_mode && config.heads && config.spt)
        set_geometry(config.heads, config.spt);
    else
        wr(6, dev_base);
}

// ---------------------------------------------------------------------------
//  Init / Reset
// ---------------------------------------------------------------------------

void ide_hw_init(void) {
    recursive_mutex_init(&ide_mtx);

    // SIO-controlled output pins
    uint32_t sio_out = (1 << IDE_DIR) | (1 << IDE_DIR1) |
                       (1 << IDE_OE)  | (1 << IDE_OE1)  |
                       ADDR_MASK |
                       (1 << IDE_RESET) |
                       (1 << IDE_CS0) | (1 << IDE_CS1);

    gpio_init_mask(sio_out);
    gpio_set_dir_out_masked(sio_out);

    // INTRQ — active-high from drive, idle low
    gpio_init(IDE_INTRQ);
    gpio_set_dir(IDE_INTRQ, GPIO_IN);
    gpio_pull_down(IDE_INTRQ);

    // IORDY — active-high when ready, pulled up as fallback
    gpio_init(IDE_IORDY);
    gpio_set_dir(IDE_IORDY, GPIO_IN);
    gpio_pull_up(IDE_IORDY);
    // Force IORDY HIGH so PIO wait instructions never block during init
    gpio_set_inover(IDE_IORDY, GPIO_OVERRIDE_HIGH);

    // Data bus pads: no pulls (PIO owns these pins after ide_pio_init)
    for (int i = 0; i < 16; i++) gpio_disable_pulls(i);

    // Bring up PIO state machines
    ide_pio_init();

    gpio_put(IDE_RESET, 1);
    bus_idle();

    // nIEN=0: allow INTRQ from the drive
    wr_ctl(0x00);

    // Restore IORDY based on config (override was HIGH for safe init)
    ide_set_iordy(config.iordy_enabled);
}

void ide_reset_drive(void) {
    ide_lock();
    // Force IORDY HIGH during reset — drive holds it LOW during POST
    ide_set_iordy(false);

    gpio_put(IDE_RESET, 0);
    sleep_ms(50);
    gpio_put(IDE_RESET, 1);
    sleep_ms(100);
    wr_ctl(0x00);
    wait_ready(10000, true, NULL);

    // Recalibrate — seek heads to track 0 (required by some pre-ATA drives)
    wr(6, dev_base);
    wr(7, 0x10);
    wait_ready(10000, true, NULL);

    // Hardware reset also clears INITIALIZE DEVICE PARAMETERS
    if (!config.use_lba_mode && config.heads && config.spt)
        set_geometry(config.heads, config.spt);

    // Restore IORDY to config setting — drive is ready for normal operation
    ide_set_iordy(config.iordy_enabled);
    ide_unlock();
}

void ide_soft_reset(void) {
    ide_lock();
    soft_reset();
    ide_unlock();
}

uint8_t ide_probe_devices(uint8_t only) {
    ide_lock();
    ide_set_iordy(false);

    // Single hardware reset — both devices see it
    gpio_put(IDE_RESET, 0);
    sleep_ms(50);
    gpio_put(IDE_RESET, 1);
    sleep_ms(2000);           // generous POST delay (not status-based)
    wr_ctl(0x00);             // nIEN=0

    static const uint8_t addrs[] = {0xA0, 0xB0};
    uint8_t found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        if (only && addrs[i] != only) continue;
        wr(6, addrs[i]);                  // select device
        busy_wait_us_32(50);              // let selection settle

        // Floating bus filter: 0xFF means no device (bus floats high)
        uint8_t st = rd(7);
        if (st == 0xFF) continue;

        // Device present — wait for BSY to clear (not DRDY; some older
        // drives won't assert DRDY until after INITIALIZE DRIVE PARAMETERS)
        dev_base = addrs[i];
        if (!wait_ready(10000, false, &st)) continue;

        // Recalibrate (required by some pre-ATA drives)
        wr(6, addrs[i]);
        wr(7, 0x10);
        wait_ready(10000, true, NULL);
        found = addrs[i];
    }

    ide_set_iordy(config.iordy_enabled);
    ide_unlock();
    return found;
}

bool ide_wait_until_ready(uint32_t timeout_ms) {
    ide_lock();
    bool ok = wait_ready(timeout_ms, true, NULL);
    ide_unlock();
    return ok;
}

bool ide_set_geometry(uint8_t heads, uint8_t spt) {
    if (heads == 0 || spt == 0) return false;
    ide_lock();
    bool ok = set_geometry(heads, spt);
    ide_unlock();
    return ok;
}

void ide_drain_sector(void) {
    ide_lock();
    drain_sector();
    ide_unlock();
}

void ide_read_taskfile(uint8_t tf[8]) {
    ide_lock();
    for (int i = 1; i <= 7; i++) tf[i] = rd(i);
    ide_unlock();
}

// ---------------------------------------------------------------------------
//  Generic command engine
// ---------------------------------------------------------------------------

int ide_exec(ide_cmd_t *c) {
    c->status = c->error = 0;
    c->out_count = c->out_lba_lo = c->out_lba_mid = c->out_lba_hi = c->out_device = 0;
    c->out_hob_count = c->out_hob_lba_lo = c->out_hob_lba_mid = c->out_hob_lba_hi = 0;
    c->tf_valid = c->err_data = c->recovered = false;
    c->done = 0;
    c->elapsed_us = 0;

    uint32_t limit_us = (c->timeout_ms ? c->timeout_ms : 1000) * 1000u;
    uint8_t dh = dev_base | (c->device & 0x4F);
    uint8_t st = 0;
    int res = IDE_OK;
    bool leftover = false;

    ide_lock();

    // Select the device and wait until it can take a command.
    wr(6, dh);
    busy_wait_us_32(1);
    if (!wait_ready(c->ready_ms ? c->ready_ms : 5000, c->flags & IDE_F_NEED_DRDY, &st)) {
        c->status = st;
        c->result = IDE_NOT_READY;
        ide_unlock();
        return IDE_NOT_READY;
    }
    if (st & ATA_ST_DRQ) drain_sector();          // stranded data from an earlier command

    if (c->ext) {
        wr(1, c->hob_feature);
        wr(2, c->hob_count);
        wr(3, c->hob_lba_lo);
        wr(4, c->hob_lba_mid);
        wr(5, c->hob_lba_hi);
    }
    wr(1, c->feature);
    wr(2, c->count);
    wr(3, c->lba_lo);
    wr(4, c->lba_mid);
    wr(5, c->lba_hi);
    wr(6, dh);

    uint32_t t0 = time_us_32();
    wr(7, c->command);
    rd_alt();                                     // >= 400 ns before STATUS is valid

    uint16_t *w = (uint16_t *)c->buf;

    if (c->proto == IDE_PROTO_PIO_IN) {
        for (uint32_t b = 0; b < c->sectors; b++) {
            int r = wait_drq(limit_us, c->flags & IDE_F_TAKE_ERR_DATA, &st);
            if (r < 0) { res = IDE_TIMEOUT; break; }
            if (r == 0) { res = IDE_ATA_ERROR; break; }
            if (!data_in(w + b * 256, 256)) { res = IDE_BUS_FAULT; break; }
            if (c->ecc_len && b == c->sectors - 1) {
                // READ LONG: vendor ECC bytes follow as 8-bit transfers on DD0-7
                for (uint8_t i = 0; i < c->ecc_len; i++)
                    c->buf[(b + 1) * 512 + i] = rd(0);
            }
            if (st & (ATA_ST_ERR | ATA_ST_DF)) { c->err_data = true; res = IDE_ATA_ERROR; break; }
            c->done = b + 1;
        }
        if (res == IDE_OK) {
            if (wait_not_busy(limit_us, &st) < 0) res = IDE_TIMEOUT;
            else if (st & (ATA_ST_ERR | ATA_ST_DF)) res = IDE_ATA_ERROR;
            else if (st & ATA_ST_DRQ) { leftover = true; res = IDE_PROTOCOL; }
        }
    } else if (c->proto == IDE_PROTO_PIO_OUT) {
        for (uint32_t b = 0; b < c->sectors; b++) {
            int r = wait_drq(limit_us, false, &st);
            if (r < 0) { res = IDE_TIMEOUT; break; }
            if (r == 0) { res = IDE_ATA_ERROR; break; }
            if (!data_out(w + b * 256, 256)) { res = IDE_BUS_FAULT; break; }
            c->done = b;                          // committed once the drive moves on
        }
        if (res == IDE_OK) {
            if (wait_not_busy(limit_us, &st) < 0) res = IDE_TIMEOUT;
            else if (st & (ATA_ST_ERR | ATA_ST_DF)) res = IDE_ATA_ERROR;
            else if (st & ATA_ST_DRQ) { leftover = true; res = IDE_PROTOCOL; }
            else c->done = c->sectors;
        }
    } else {
        if (wait_not_busy(limit_us, &st) < 0) res = IDE_TIMEOUT;
        else if (st & (ATA_ST_ERR | ATA_ST_DF)) res = IDE_ATA_ERROR;
        else if (st & ATA_ST_DRQ) { leftover = true; res = IDE_PROTOCOL; }
    }

    c->elapsed_us = time_us_32() - t0;
    c->status = st;

    // Result registers are only meaningful with BSY=0; while BSY is set every
    // register reads back as STATUS.
    if (!(st & ATA_ST_BSY)) {
        c->error       = rd(1);
        c->out_count   = rd(2);
        c->out_lba_lo  = rd(3);
        c->out_lba_mid = rd(4);
        c->out_lba_hi  = rd(5);
        c->out_device  = rd(6);
        if (c->ext) {
            wr_ctl(0x80);                         // HOB=1: read the "previous" bytes
            c->out_hob_count   = rd(2);
            c->out_hob_lba_lo  = rd(3);
            c->out_hob_lba_mid = rd(4);
            c->out_hob_lba_hi  = rd(5);
            wr_ctl(0x00);
        }
        c->tf_valid = true;
        if (!(st & (ATA_ST_ERR | ATA_ST_DF))) c->error = 0;   // error reg is undefined without ERR
    }

    if ((res == IDE_TIMEOUT || res == IDE_BUS_FAULT || leftover) && !(c->flags & IDE_F_NO_RECOVER)) {
        soft_reset();
        c->recovered = true;
    }

    c->result = res;
    ide_unlock();
    return res;
}

bool ide_cmd_address(ide_cmd_t *c, uint32_t lba, uint32_t count, bool allow_ext) {
    if (count == 0) return false;
    c->ext = false;
    c->hob_feature = c->hob_count = c->hob_lba_lo = c->hob_lba_mid = c->hob_lba_hi = 0;

    if (config.use_lba_mode) {
        bool lba48 = config.lba_sectors > 0x0FFFFFFF;
        if (lba48 && allow_ext) {
            if (count > 65536) return false;
            c->ext = true;
            c->hob_count  = (uint8_t)((count & 0xFFFF) >> 8);
            c->hob_lba_lo = (uint8_t)(lba >> 24);
            c->count   = (uint8_t)count;
            c->lba_lo  = (uint8_t)lba;
            c->lba_mid = (uint8_t)(lba >> 8);
            c->lba_hi  = (uint8_t)(lba >> 16);
            c->device  = 0x40;
            return true;
        }
        if (count > 256 || lba + count > 0x10000000u) return false;
        c->count   = (uint8_t)count;              // 256 -> 0
        c->lba_lo  = (uint8_t)lba;
        c->lba_mid = (uint8_t)(lba >> 8);
        c->lba_hi  = (uint8_t)(lba >> 16);
        c->device  = 0x40 | ((lba >> 24) & 0x0F);
        return true;
    }

    if (!config.heads || !config.spt || config.heads > 16 || count > 256) return false;
    uint32_t tmp  = lba / config.spt;
    uint8_t  sec  = (uint8_t)(lba % config.spt) + 1;   // 1-based
    uint8_t  head = (uint8_t)(tmp % config.heads);
    uint32_t cyl  = tmp / config.heads;
    if (cyl > 0xFFFF) return false;
    c->count   = (uint8_t)count;
    c->lba_lo  = sec;
    c->lba_mid = (uint8_t)cyl;
    c->lba_hi  = (uint8_t)(cyl >> 8);
    c->device  = head & 0x0F;
    return true;
}

bool ide_cmd_result_lba(const ide_cmd_t *c, uint32_t *lba) {
    if (!c->tf_valid) return false;
    if (c->ext) {
        *lba = (uint32_t)c->out_lba_lo | ((uint32_t)c->out_lba_mid << 8) |
               ((uint32_t)c->out_lba_hi << 16) | ((uint32_t)c->out_hob_lba_lo << 24);
        return true;
    }
    if (c->device & 0x40) {
        *lba = (uint32_t)c->out_lba_lo | ((uint32_t)c->out_lba_mid << 8) |
               ((uint32_t)c->out_lba_hi << 16) | ((uint32_t)(c->out_device & 0x0F) << 24);
        return true;
    }
    if (!config.heads || !config.spt || c->out_lba_lo == 0) return false;
    uint32_t cyl = c->out_lba_mid | ((uint32_t)c->out_lba_hi << 8);
    uint32_t head = c->out_device & 0x0F;
    *lba = (cyl * config.heads + head) * config.spt + (c->out_lba_lo - 1);
    return true;
}

// ---------------------------------------------------------------------------
//  Convenience wrappers used by the menu
// ---------------------------------------------------------------------------

bool ide_identify(uint16_t *buf) {
    ide_cmd_t c = {0};
    c.command = 0xEC;
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = (uint8_t *)buf;
    c.sectors = 1;
    c.flags = IDE_F_NEED_DRDY;
    c.ready_ms = 1000;
    c.timeout_ms = 5000;
    return ide_exec(&c) == IDE_OK;
}

uint8_t ide_seek_read_one(uint32_t target, bool lba) {
    static uint8_t scratch[512];
    ide_cmd_t c = {0};
    if (lba) {
        bool use_lba48 = config.lba_sectors > 0x0FFFFFFF;
        c.count = 1;
        c.lba_lo = (uint8_t)target; c.lba_mid = (uint8_t)(target >> 8); c.lba_hi = (uint8_t)(target >> 16);
        if (use_lba48) { c.ext = true; c.hob_lba_lo = (uint8_t)(target >> 24); c.device = 0x40; }
        else c.device = 0x40 | ((target >> 24) & 0x0F);
        c.command = use_lba48 ? 0x24 : 0x20;
    } else {
        c.count = 1;
        c.lba_lo = 1;                              // sector 1 (1-based)
        c.lba_mid = (uint8_t)target; c.lba_hi = (uint8_t)(target >> 8);
        c.device = 0;                              // head 0, CHS
        c.command = 0x20;
    }
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = scratch;
    c.sectors = 1;
    c.timeout_ms = 100;
    c.flags = IDE_F_NO_RECOVER | IDE_F_TAKE_ERR_DATA;
    ide_exec(&c);
    return c.status;
}
