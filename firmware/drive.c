#include "drive.h"
#include "config.h"
#include "pico/time.h"
#include <string.h>

drive_state_t drive;
drive_stats_t drive_stats;

static errlog_entry_t errlog[ERRLOG_SIZE];
static uint32_t errlog_seq = 0;      // total failures logged since boot/clear

// ---------------------------------------------------------------------------
//  Identity / geometry helpers
// ---------------------------------------------------------------------------

static void ata_string(const uint16_t *id, int word, int nwords, char *out) {
    int n = 0;
    for (int i = 0; i < nwords; i++) {
        out[n++] = (char)(id[word + i] >> 8);
        out[n++] = (char)(id[word + i] & 0xFF);
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) out[--n] = '\0';
    // leading spaces are common in serial numbers
    int lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, (size_t)(n - lead + 1));
}

void drive_note_identify(const uint16_t *id) {
    memcpy(drive.id, id, sizeof(drive.id));
    ata_string(id, 27, 20, drive.model);
    ata_string(id, 10, 10, drive.serial);
    ata_string(id, 23, 4, drive.fw);
    drive.present = true;
    drive.identified = true;
    drive.forced = false;
}

void drive_note_forced(void) {
    memset(drive.id, 0, sizeof(drive.id));
    strcpy(drive.model, "Manually Forced Drive");
    drive.serial[0] = drive.fw[0] = '\0';
    drive.identified = false;
    drive.forced = true;
}

void drive_clear_identity(void) {
    drive.identified = false;
    drive.forced = false;
    drive.model[0] = drive.serial[0] = drive.fw[0] = '\0';
}

bool drive_geometry_valid(void) {
    if (config.use_lba_mode) return config.lba_sectors > 0;
    return config.cyls > 0 && config.heads > 0 && config.heads <= 16 && config.spt > 0;
}

uint64_t drive_total_sectors(void) {
    if (!drive_geometry_valid()) return 0;
    if (config.use_lba_mode) return config.lba_sectors;
    return (uint64_t)config.cyls * config.heads * config.spt;
}

uint8_t drive_ecc_len(void) {
    if (config.ecc_len) return config.ecc_len;
    if (drive.identified && drive.id[22] > 0 && drive.id[22] <= 64) return (uint8_t)drive.id[22];
    return 4;
}

int drive_result_from_ide(int r) {
    switch (r) {
    case IDE_OK:        return R_OK;
    case IDE_ATA_ERROR: return R_ATA_ERROR;
    case IDE_TIMEOUT:   return R_TIMEOUT;
    case IDE_NOT_READY: return R_NOT_READY;
    case IDE_BUS_FAULT: return R_BUS_FAULT;
    default:            return R_PROTOCOL;
    }
}

// ---------------------------------------------------------------------------
//  Geometry / detect / mount
// ---------------------------------------------------------------------------

// Remember which drive the configured geometry belongs to ("" when there is
// no IDENTIFY data, e.g. forced manual geometry).
static void set_geometry_owner(void) {
    _Static_assert(sizeof(config.geo_model) == sizeof(drive.model) &&
                   sizeof(config.geo_serial) == sizeof(drive.serial), "owner fields must match drive_state_t");
    memset(config.geo_model, 0, sizeof(config.geo_model));
    memset(config.geo_serial, 0, sizeof(config.geo_serial));
    if (drive.identified) {                           // both NUL-terminated by ata_string()
        memcpy(config.geo_model, drive.model, sizeof(config.geo_model));
        memcpy(config.geo_serial, drive.serial, sizeof(config.geo_serial));
    }
}

int drive_geo_identity(void) {
    return config_geo_identity(&config, drive.model, drive.serial, drive.identified ? drive.id : NULL);
}

int drive_apply_geometry(uint8_t mode, uint16_t c, uint8_t h, uint8_t s) {
    const uint16_t *id = drive.id;
    bool lba = false;
    uint64_t lba_sectors = 0;

    switch (mode) {
    case GEO_NORMAL:
        if (!drive.identified) return R_IDENTIFY_FAILED;
        c = id[1]; h = (uint8_t)id[3]; s = (uint8_t)id[6];
        break;
    case GEO_LARGE:
        if (!drive.identified) return R_IDENTIFY_FAILED;
        config_large_chs(id[1], (uint8_t)id[3], &c, &h);
        s = (uint8_t)id[6];
        break;
    case GEO_LBA: {
        if (!drive.identified) return R_IDENTIFY_FAILED;
        if (!(id[49] & 0x0200)) return R_UNSUPPORTED;
        uint64_t l48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[101] << 16) | (uint64_t)id[100];
        uint32_t l28 = id[60] | ((uint32_t)id[61] << 16);
        lba_sectors = ((id[83] & (1 << 10)) && l48) ? l48 : l28;
        if (!lba_sectors) return R_UNSUPPORTED;
        lba = true;
        c = id[1]; h = (uint8_t)id[3]; s = (uint8_t)id[6];
        break;
    }
    case GEO_MANUAL:
        if (!config_chs_valid(c, h, s)) return R_BAD_ARG;
        break;
    default:
        return R_BAD_ARG;
    }

    if (!lba && (c == 0 || h == 0 || h > 16 || s == 0)) return R_BAD_ARG;

    ide_lock();
    config.use_lba_mode = lba;
    config.lba_sectors = lba_sectors;
    config.cyls = c;
    config.heads = h;
    config.spt = s;
    config.geo_mode = mode;
    set_geometry_owner();
    if (!lba) ide_set_geometry(h, s);   // INITIALIZE DEVICE PARAMETERS
    ide_unlock();
    return R_OK;
}

void drive_clear_geometry(void) {
    ide_lock();
    config.use_lba_mode = false;
    config.lba_sectors = 0;
    config.cyls = 0;
    config.heads = 0;
    config.spt = 0;
    config.geo_mode = GEO_NONE;
    set_geometry_owner();
    ide_unlock();
}

int drive_detect(uint8_t only_dev, uint8_t geo_mode, uint16_t c, uint8_t h, uint8_t s, uint8_t flags) {
    if (is_mounted) return R_MOUNTED;

    ide_lock();
    uint8_t found;
    if (flags & DETECT_SKIP_PROBE) {
        found = only_dev ? only_dev : config.dev_base;
        ide_select_device(found);
        ide_write_reg(6, found);
        busy_wait_us_32(50);
        if (ide_read_reg(7) == 0xFF) found = 0;   // floating bus: nothing there
    } else {
        found = ide_probe_devices(only_dev);
    }

    drive.present = (found != 0);
    if (!found) {
        drive_clear_identity();
        ide_unlock();
        return R_NO_DRIVE;
    }
    config.dev_base = found;
    ide_select_device(found);

    uint16_t id[256];
    ide_cmd_t ic = {0};
    ic.command = 0xEC;
    ic.proto = IDE_PROTO_PIO_IN;
    ic.buf = (uint8_t *)id;
    ic.sectors = 1;
    ic.flags = IDE_F_NEED_DRDY;
    ic.ready_ms = 1000;
    ic.timeout_ms = 5000;
    bool ok = ide_exec(&ic) == IDE_OK;
    drive_record(LOGOP_IDENTIFY, &ic, 0, 1);

    int r = R_OK;
    if (ok) {
        drive_note_identify(id);
    } else if ((flags & DETECT_FORCE) && geo_mode == GEO_MANUAL) {
        drive_note_forced();
    } else {
        drive_clear_identity();
        ide_unlock();
        return R_IDENTIFY_FAILED;
    }

    if (geo_mode == GEO_AUTO) geo_mode = (drive.id[49] & 0x0200) ? GEO_LBA : GEO_NORMAL;
    if (geo_mode != GEO_KEEP) r = drive_apply_geometry(geo_mode, c, h, s);
    else if (!config.use_lba_mode && config.heads && config.spt)
        ide_set_geometry(config.heads, config.spt);   // the probe's RESET cleared INIT PARAMS
    ide_unlock();

    if (r == R_OK && (flags & DETECT_MOUNT)) r = drive_mount_ex((flags & DETECT_MOUNT_FORCE) ? MOUNT_FORCE : 0);
    return r;
}

#define MOUNT_READY_MS 3000

// Before exposing the drive: something must answer on the bus, it must be
// ready within MOUNT_READY_MS (a drive that never gets ready would stall
// every USB read, and with it the serial menu), and the geometry must
// belong to it.  A drive not
// identified since boot is asked for IDENTIFY first, so a mount straight
// after power-up compares the saved geometry with the drive really attached.
static int mount_checks(void) {
    ide_lock();
    uint8_t dev = ide_selected_device();
    ide_write_reg(6, dev);
    busy_wait_us_32(50);
    uint8_t st = ide_read_alt_status();
    if (st == 0xFF) {                                 // floating bus
        drive.present = false;
        ide_unlock();
        return R_NO_DRIVE;
    }
    // USB reads are issued with IDE_F_NEED_DRDY: without BSY=0 + DRDY=1 every
    // one of them would wait out its 5 s ready timeout and fail.
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while ((st & (ATA_ST_BSY | ATA_ST_DRDY)) != ATA_ST_DRDY) {
        if (to_ms_since_boot(get_absolute_time()) - t0 >= MOUNT_READY_MS) {
            ide_unlock();
            return R_NOT_READY;
        }
        busy_wait_us_32(1000);
        st = ide_read_alt_status();
    }
    drive.present = true;

    if (!drive.identified && !drive.forced) {
        uint16_t id[256];
        ide_cmd_t ic = {0};
        ic.command = 0xEC;
        ic.proto = IDE_PROTO_PIO_IN;
        ic.buf = (uint8_t *)id;
        ic.sectors = 1;
        ic.flags = IDE_F_NEED_DRDY;
        ic.ready_ms = 1000;
        ic.timeout_ms = 5000;
        if (ide_exec(&ic) == IDE_OK) {
            drive_note_identify(id);
            drive.api_gen++;
        }
        drive_record(LOGOP_IDENTIFY, &ic, 0, 1);
        // No IDENTIFY: nothing to compare against; mount on the saved geometry.
    }
    ide_unlock();

    int idv = drive_geo_identity();
    if (idv == GEO_ID_MISMATCH) return R_GEO_MISMATCH;
    if (idv == GEO_ID_MATCH && !config.geo_model[0] && !config.geo_serial[0])
        set_geometry_owner();                         // older settings: owner confirmed now
    return R_OK;
}

int drive_mount_ex(uint8_t flags) {
    if (!drive_geometry_valid()) return R_NO_GEOMETRY;
    if (is_mounted) return R_OK;
    if (!(flags & MOUNT_FORCE)) {
        int r = mount_checks();
        if (r != R_OK) return r;
    }
    is_mounted = true;
    media_changed_waiting = true;
    return R_OK;
}

int drive_mount(void) {
    return drive_mount_ex(0);
}

int drive_unmount(void) {
    if (is_mounted) {
        is_mounted = false;
        media_changed_waiting = true;
    }
    return R_OK;
}

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------

void drive_record(uint8_t op, const ide_cmd_t *c, uint32_t lba, uint32_t count) {
    bool failed = c->result != IDE_OK;
    if (c->result == IDE_TIMEOUT) drive_stats.timeouts++;
    if (c->recovered) drive_stats.recoveries++;
    if (!failed) return;

    errlog_entry_t *x = &errlog[errlog_seq % ERRLOG_SIZE];
    memset(x, 0, sizeof(*x));
    x->seq        = ++errlog_seq;
    x->t_ms       = to_ms_since_boot(get_absolute_time());
    x->op         = op;
    x->cmd        = c->command;
    x->result     = (int8_t)c->result;
    x->status     = c->status;
    x->error      = c->error;
    x->tf_valid   = c->tf_valid;
    x->err_data   = c->err_data;
    x->recovered  = c->recovered;
    x->lba        = lba;
    x->count      = (uint16_t)(count > 0xFFFF ? 0xFFFF : count);
    x->done       = (uint16_t)(c->done > 0xFFFF ? 0xFFFF : c->done);
    x->elapsed_ms = c->elapsed_us / 1000u;
    x->timeout_ms = c->timeout_ms;
    uint32_t fl;
    x->fail_lba   = (op != LOGOP_IDENTIFY && op != LOGOP_PASSTHRU && ide_cmd_result_lba(c, &fl)) ? fl : lba + c->done;
    if (c->tf_valid) {
        x->tf[0] = c->out_count;  x->tf[1] = c->out_lba_lo; x->tf[2] = c->out_lba_mid;
        x->tf[3] = c->out_lba_hi; x->tf[4] = c->out_device;
    }
}

uint32_t drive_log_total(void) { return errlog_seq; }
uint32_t drive_log_count(void) { return errlog_seq < ERRLOG_SIZE ? errlog_seq : ERRLOG_SIZE; }

const errlog_entry_t *drive_log_get(uint32_t i) {
    uint32_t n = drive_log_count();
    if (i >= n) return NULL;
    return &errlog[(errlog_seq - n + i) % ERRLOG_SIZE];
}

const errlog_entry_t *drive_last_failure(void) {
    if (!errlog_seq) return NULL;
    return &errlog[(errlog_seq - 1) % ERRLOG_SIZE];
}

void drive_log_clear(void)   { errlog_seq = 0; }
void drive_stats_clear(void) { memset(&drive_stats, 0, sizeof(drive_stats)); }

// ---------------------------------------------------------------------------
//  Mass-storage sector I/O
// ---------------------------------------------------------------------------

// An LBA the configured geometry can't express: log it so the sense data
// describes this failure rather than an older one.
static int32_t address_fail(uint8_t op, uint32_t lba, uint32_t count) {
    ide_cmd_t c = {0};
    c.result = IDE_PROTOCOL;
    drive_record(op, &c, lba, count);
    return -1;
}

int32_t drive_read(uint32_t lba, uint32_t count, uint8_t *buf) {
    ide_cmd_t c = {0};
    if (!ide_cmd_address(&c, lba, count, true)) return address_fail(LOGOP_READ, lba, count);
    c.command = c.ext ? 0x24 : (config.no_retry ? 0x21 : 0x20);
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = buf;
    c.sectors = count;
    c.flags = IDE_F_NEED_DRDY;
    c.timeout_ms = config.read_timeout_ms;
    drive_stats.read_cmds++;
    int r = ide_exec(&c);
    drive_stats.read_sectors += c.done;
    if (r != IDE_OK) drive_stats.read_errors++;
    drive_record(LOGOP_READ, &c, lba, count);
    return r == IDE_OK ? (int32_t)(count * 512) : -1;
}

int32_t drive_write(uint32_t lba, uint32_t count, const uint8_t *buf) {
    ide_cmd_t c = {0};
    if (!ide_cmd_address(&c, lba, count, true)) return address_fail(LOGOP_WRITE, lba, count);
    c.command = c.ext ? 0x34 : 0x30;
    c.proto = IDE_PROTO_PIO_OUT;
    c.buf = (uint8_t *)buf;
    c.sectors = count;
    c.flags = IDE_F_NEED_DRDY;
    c.timeout_ms = config.write_timeout_ms;
    drive_stats.write_cmds++;
    int r = ide_exec(&c);
    drive_stats.write_sectors += c.done;
    if (r != IDE_OK) drive_stats.write_errors++;
    drive_record(LOGOP_WRITE, &c, lba, count);
    return r == IDE_OK ? (int32_t)(count * 512) : -1;
}
