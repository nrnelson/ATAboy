// ATAboy automation API — vendor SCSI command 0xC0.
//
// Every request is a 16-byte CDB: C0 <op> 'A' 'B' <12 bytes of arguments>,
// data-in.  Every reply starts with a 16-byte header followed by either a
// JSON document (state, config, logs) or a binary block (sector data).
// Device-level outcomes are reported in-band (header result code + body);
// CHECK CONDITION is only used for malformed CDBs.  See docs/API.md.

#include "api.h"
#include "drive.h"
#include "config.h"
#include "json.h"
#include "sense.h"
#include "version.h"
#include "ide_pio.h"
#include "tusb.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include <string.h>

// Sub-operations
#define OP_INFO          0x00
#define OP_STATUS        0x01
#define OP_CONFIG_GET    0x02
#define OP_CONFIG_SET    0x03
#define OP_CONFIG_SAVE   0x04
#define OP_CONFIG_DEFAULTS 0x05
#define OP_DETECT        0x10
#define OP_GEOMETRY      0x11
#define OP_MOUNT         0x12
#define OP_UNMOUNT       0x13
#define OP_RESET         0x14
#define OP_IDENTIFY      0x20
#define OP_TASKFILE      0x21
#define OP_READ          0x22
#define OP_READ_LONG     0x23
#define OP_VERIFY        0x24
#define OP_CORRUPT       0x25
#define OP_LOG           0x30
#define OP_LOG_CLEAR     0x31
#define OP_ARM           0x40
#define OP_REBOOT        0x41

// Config keys (OP_CONFIG_SET)
#define KEY_WRITE_PROTECT 1
#define KEY_AUTO_MOUNT    2
#define KEY_IORDY         3
#define KEY_INTRQ         4
#define KEY_READ_TIMEOUT  5
#define KEY_WRITE_TIMEOUT 6
#define KEY_ECC_LEN       7
#define KEY_NO_RETRY      8
#define KEY_DEVICE        9

#define HDR_LEN     16
#define RES_LEN     32           // binary ATA result block
#define CT_NONE     0
#define CT_JSON     1
#define CT_BINARY   2

#define ARM_WINDOW_MS   10000
#define VERIFY_BUDGET_US 10000000u   // one VERIFY request runs at most ~10 s

static uint8_t  cur_op;
static uint32_t armed_until_ms = 0;
static bool     armed = false;
static int      reboot_mode = -1;     // -1 none, 0 normal, 1 BOOTSEL
static uint32_t reboot_at_ms;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

bool api_is_armed(void) {
    if (armed && (int32_t)(now_ms() - armed_until_ms) >= 0) armed = false;
    return armed;
}

bool api_take_arm(void) {
    bool a = api_is_armed();
    armed = false;
    return a;
}

void api_poll(void) {
    if (reboot_mode < 0 || (int32_t)(now_ms() - reboot_at_ms) < 0) return;
    if (reboot_mode == 1) reset_usb_boot(0, 0);   // BOOTSEL: UF2 drive + PICOBOOT
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const char *result_name(int r) {
    static const char *const names[] = {
        "ok", "bad_subop", "bad_arg", "no_drive", "not_mounted", "write_protected",
        "not_armed", "ata_error", "timeout", "not_ready", "bus_fault", "no_geometry",
        "mounted", "identify_failed", "unsupported", "protocol", "alloc_too_small",
    };
    return (r >= 0 && r < (int)(sizeof(names) / sizeof(names[0]))) ? names[r] : "unknown";
}

static const char *geo_name(uint8_t m) {
    switch (m) {
    case GEO_NORMAL: return "normal";
    case GEO_LARGE:  return "large";
    case GEO_LBA:    return "lba";
    case GEO_MANUAL: return "manual";
    default:         return "none";
    }
}

static const char *op_name(uint8_t op) {
    switch (op) {
    case LOGOP_READ:      return "read";
    case LOGOP_WRITE:     return "write";
    case LOGOP_READ_LONG: return "read_long";
    case LOGOP_VERIFY:    return "verify";
    case LOGOP_PASSTHRU:  return "passthrough";
    case LOGOP_IDENTIFY:  return "identify";
    case LOGOP_READ_DIAG: return "read_diag";
    case LOGOP_WRITE_LONG: return "write_long";
    default:           return "unknown";
    }
}

static const char *ide_result_name(int r) {
    switch (r) {
    case IDE_OK:        return "ok";
    case IDE_ATA_ERROR: return "ata_error";
    case IDE_TIMEOUT:   return "timeout";
    case IDE_NOT_READY: return "not_ready";
    case IDE_BUS_FAULT: return "bus_fault";
    default:            return "protocol";
    }
}

static uint8_t hdr_flags(void) {
    return (is_mounted ? 0x01 : 0) | (config.drive_write_protected ? 0x02 : 0) |
           (drive.identified ? 0x04 : 0) | (api_is_armed() ? 0x08 : 0) |
           (drive_geometry_valid() ? 0x10 : 0);
}

static int32_t finish(uint8_t *buf, int result, uint8_t ctype, uint32_t payload_len) {
    buf[0] = 'A'; buf[1] = 'B';
    buf[2] = API_PROTO_VERSION;
    buf[3] = cur_op;
    buf[4] = (uint8_t)result;
    buf[5] = ctype;
    put_be16(&buf[6], (uint16_t)payload_len);
    put_be32(&buf[8], now_ms());
    buf[12] = hdr_flags();
    buf[13] = buf[14] = buf[15] = 0;
    return (int32_t)(HDR_LEN + payload_len);
}

// ---------------------------------------------------------------------------
//  JSON sections
// ---------------------------------------------------------------------------

static void json_config(json_t *j, const char *key) {
    j_obj(j, key);
    j_bool(j, "write_protect", config.drive_write_protected);
    j_bool(j, "auto_mount", config.auto_mount);
    j_bool(j, "iordy", config.iordy_enabled);
    j_bool(j, "intrq", config.intrq_enabled);
    j_uint(j, "read_timeout_ms", config.read_timeout_ms);
    j_uint(j, "write_timeout_ms", config.write_timeout_ms);
    j_uint(j, "ecc_len", config.ecc_len);
    j_uint(j, "ecc_len_effective", drive_ecc_len());
    j_bool(j, "no_retry", config.no_retry);
    j_str(j, "device", config.dev_base == 0xB0 ? "slave" : "master");
    j_end(j);
}

static void json_log_entry(json_t *j, const char *key, const errlog_entry_t *e) {
    char sb[48], eb[48];
    j_obj(j, key);
    j_uint(j, "seq", e->seq);
    j_uint(j, "t_ms", e->t_ms);
    j_str(j, "op", op_name(e->op));
    j_uint(j, "cmd", e->cmd);
    j_str(j, "result", ide_result_name(e->result));
    j_uint(j, "lba", e->lba);
    j_uint(j, "count", e->count);
    j_uint(j, "done", e->done);
    j_uint(j, "fail_lba", e->fail_lba);
    j_uint(j, "status", e->status);
    j_str(j, "status_bits", ata_status_bits(e->status, sb));
    j_uint(j, "error", e->error);
    j_str(j, "error_bits", ata_error_bits(e->error, eb));
    j_bool(j, "tf_valid", e->tf_valid);
    j_bool(j, "err_data", e->err_data);
    j_bool(j, "recovered", e->recovered);
    j_uint(j, "elapsed_ms", e->elapsed_ms);
    j_uint(j, "timeout_ms", e->timeout_ms);
    if (e->tf_valid) {
        j_arr(j, "tf");
        for (int i = 0; i < 5; i++) j_uint(j, NULL, e->tf[i]);
        j_end(j);
    } else {
        j_null(j, "tf");
    }
    j_end(j);
}

static void json_status_body(json_t *j) {
    char sb[48];
    const uint16_t *id = drive.id;

    j_str(j, "fw", FW_VERSION);
    j_uint(j, "uptime_ms", now_ms());
    j_bool(j, "mounted", is_mounted);
    j_bool(j, "write_protect", config.drive_write_protected);
    j_bool(j, "armed", api_is_armed());

    j_obj(j, "drive");
    j_bool(j, "present", drive.present);
    j_bool(j, "identified", drive.identified);
    j_bool(j, "forced", drive.forced);
    j_str(j, "device", config.dev_base == 0xB0 ? "slave" : "master");
    j_str(j, "model", drive.model);
    j_str(j, "serial", drive.serial);
    j_str(j, "firmware", drive.fw);
    if (drive.identified) {
        uint32_t l28 = id[60] | ((uint32_t)id[61] << 16);
        uint64_t l48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[101] << 16) | (uint64_t)id[100];
        j_arr(j, "default_chs");
        j_uint(j, NULL, id[1]); j_uint(j, NULL, id[3]); j_uint(j, NULL, id[6]);
        j_end(j);
        j_bool(j, "lba", (id[49] & 0x0200) != 0);
        j_uint(j, "lba28_sectors", l28);
        j_bool(j, "lba48", (id[83] & (1 << 10)) != 0);
        j_uint(j, "lba48_sectors", l48);
        j_uint(j, "ecc_bytes", id[22]);
        j_uint(j, "ata_major", id[80] == 0xFFFF ? 0 : id[80]);
    }
    j_end(j);

    j_obj(j, "geometry");
    bool gv = drive_geometry_valid();
    j_bool(j, "valid", gv);
    j_str(j, "mode", geo_name(config.geo_mode));
    j_str(j, "addressing", !gv ? "none" : !config.use_lba_mode ? "chs" :
                           (config.lba_sectors > 0x0FFFFFFF ? "lba48" : "lba28"));
    j_uint(j, "cyls", config.cyls);
    j_uint(j, "heads", config.heads);
    j_uint(j, "spt", config.spt);
    j_uint(j, "sectors", drive_total_sectors());
    j_end(j);

    j_obj(j, "bus");
    uint8_t alt = ide_read_alt_status();
    j_uint(j, "status", alt);
    j_str(j, "status_bits", ata_status_bits(alt, sb));
    j_uint(j, "pio_faults", ide_pio_faults);
    j_end(j);

    json_config(j, "config");

    j_obj(j, "stats");
    j_uint(j, "read_cmds", drive_stats.read_cmds);
    j_uint(j, "read_sectors", drive_stats.read_sectors);
    j_uint(j, "read_errors", drive_stats.read_errors);
    j_uint(j, "write_cmds", drive_stats.write_cmds);
    j_uint(j, "write_sectors", drive_stats.write_sectors);
    j_uint(j, "write_errors", drive_stats.write_errors);
    j_uint(j, "timeouts", drive_stats.timeouts);
    j_uint(j, "recoveries", drive_stats.recoveries);
    j_uint(j, "passthru_cmds", drive_stats.passthru_cmds);
    j_uint(j, "passthru_errors", drive_stats.passthru_errors);
    j_uint(j, "passthru_blocked", drive_stats.passthru_blocked);
    j_uint(j, "api_cmds", drive_stats.api_cmds);
    j_end(j);

    j_obj(j, "errors");
    j_uint(j, "total", drive_log_total());
    j_uint(j, "retained", drive_log_count());
    const errlog_entry_t *last = drive_last_failure();
    if (last) json_log_entry(j, "last", last);
    else j_null(j, "last");
    j_end(j);
}

// Reply: {"result": "...", "message": "...", <status fields>}
static int32_t reply_status(uint8_t *buf, uint32_t cap, int result, const char *msg) {
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN);
    j_obj(&j, NULL);
    j_str(&j, "result", result_name(result));
    if (msg) j_str(&j, "message", msg);
    json_status_body(&j);
    j_end(&j);
    return finish(buf, result, CT_JSON, j.len);
}

static int32_t reply_msg(uint8_t *buf, uint32_t cap, int result, const char *msg) {
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN);
    j_obj(&j, NULL);
    j_str(&j, "result", result_name(result));
    if (msg) j_str(&j, "message", msg);
    j_end(&j);
    return finish(buf, result, CT_JSON, j.len);
}

static int32_t reply_config(uint8_t *buf, uint32_t cap, int result, const char *msg) {
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN);
    j_obj(&j, NULL);
    j_str(&j, "result", result_name(result));
    if (msg) j_str(&j, "message", msg);
    json_config(&j, "config");
    j_end(&j);
    return finish(buf, result, CT_JSON, j.len);
}

// ---------------------------------------------------------------------------
//  Binary ATA result block (32 bytes) + data
// ---------------------------------------------------------------------------

static uint32_t put_result(uint8_t *p, const ide_cmd_t *c, uint32_t lba, uint32_t count, bool partial) {
    memset(p, 0, RES_LEN);
    put_be32(&p[0], lba);
    put_be16(&p[4], (uint16_t)count);
    put_be16(&p[6], (uint16_t)c->done);
    p[8]  = c->status;
    p[9]  = c->error;
    p[10] = c->command;
    p[11] = (c->result == IDE_TIMEOUT ? 0x01 : 0) | (c->err_data ? 0x02 : 0) |
            (c->tf_valid ? 0x04 : 0) | (c->recovered ? 0x08 : 0) |
            (c->result == IDE_BUS_FAULT ? 0x10 : 0) | (c->result == IDE_NOT_READY ? 0x20 : 0) |
            (partial ? 0x40 : 0);
    put_be32(&p[12], c->elapsed_us);
    uint32_t fl;
    if (c->result != IDE_OK) {
        if (!ide_cmd_result_lba(c, &fl)) fl = lba + c->done;
    } else fl = 0xFFFFFFFF;
    put_be32(&p[16], fl);
    p[20] = c->out_count; p[21] = c->out_lba_lo; p[22] = c->out_lba_mid;
    p[23] = c->out_lba_hi; p[24] = c->out_device;
    p[25] = c->ecc_len;
    p[26] = (uint8_t)(int8_t)c->result;
    return RES_LEN;
}

// ---------------------------------------------------------------------------
//  Sub-operation handlers
// ---------------------------------------------------------------------------

static int32_t op_info(uint8_t *buf, uint32_t cap) {
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN);
    j_obj(&j, NULL);
    j_str(&j, "result", "ok");
    j_uint(&j, "proto", API_PROTO_VERSION);
    j_str(&j, "fw", FW_VERSION);
    j_str(&j, "base", FW_BASE_VERSION);
    j_str(&j, "git", FW_GIT_HASH);
    j_str(&j, "built", __DATE__ " " __TIME__);
    j_str(&j, "board", "RP2350");
    j_str(&j, "board_id", board_id);
    j_uint(&j, "max_xfer", CFG_TUD_MSC_EP_BUFSIZE);
    j_uint(&j, "max_read_sectors", (CFG_TUD_MSC_EP_BUFSIZE - HDR_LEN - RES_LEN) / 512);
    j_obj(&j, "ops");
    j_uint(&j, "info", OP_INFO);           j_uint(&j, "status", OP_STATUS);
    j_uint(&j, "config_get", OP_CONFIG_GET); j_uint(&j, "config_set", OP_CONFIG_SET);
    j_uint(&j, "config_save", OP_CONFIG_SAVE); j_uint(&j, "config_defaults", OP_CONFIG_DEFAULTS);
    j_uint(&j, "detect", OP_DETECT);       j_uint(&j, "geometry", OP_GEOMETRY);
    j_uint(&j, "mount", OP_MOUNT);         j_uint(&j, "unmount", OP_UNMOUNT);
    j_uint(&j, "reset", OP_RESET);         j_uint(&j, "identify", OP_IDENTIFY);
    j_uint(&j, "taskfile", OP_TASKFILE);   j_uint(&j, "read", OP_READ);
    j_uint(&j, "read_long", OP_READ_LONG); j_uint(&j, "verify", OP_VERIFY);
    j_uint(&j, "corrupt", OP_CORRUPT);
    j_uint(&j, "log", OP_LOG);             j_uint(&j, "log_clear", OP_LOG_CLEAR);
    j_uint(&j, "arm", OP_ARM);             j_uint(&j, "reboot", OP_REBOOT);
    j_end(&j);
    j_arr(&j, "features");
    j_str(&j, NULL, "sat12"); j_str(&j, NULL, "sat16"); j_str(&j, NULL, "pio_in");
    j_str(&j, NULL, "pio_out"); j_str(&j, NULL, "read_through_error");
    j_str(&j, NULL, "read_long"); j_str(&j, NULL, "verify"); j_str(&j, NULL, "bootsel");
    j_str(&j, NULL, "persistent_timeouts"); j_str(&j, NULL, "no_retry_reads");
    j_str(&j, NULL, "corrupt_sector");
    j_end(&j);
    j_end(&j);
    return finish(buf, R_OK, CT_JSON, j.len);
}

static int32_t op_config_set(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint8_t key = a[0];
    uint32_t v = be32(&a[4]);
    switch (key) {
    case KEY_WRITE_PROTECT:
        if (config.drive_write_protected != (v != 0)) {
            config.drive_write_protected = (v != 0);
            if (is_mounted) media_changed_waiting = true;   // host re-reads the WP bit
        }
        break;
    case KEY_AUTO_MOUNT: config.auto_mount = (v != 0); break;
    case KEY_IORDY:      config.iordy_enabled = (v != 0); ide_set_iordy(config.iordy_enabled); break;
    case KEY_INTRQ:      config.intrq_enabled = (v != 0); break;
    case KEY_READ_TIMEOUT:
        if (v < TIMEOUT_MIN_MS || v > TIMEOUT_MAX_MS) return reply_config(buf, cap, R_BAD_ARG, "timeout must be 50..600000 ms");
        config.read_timeout_ms = v; break;
    case KEY_WRITE_TIMEOUT:
        if (v < TIMEOUT_MIN_MS || v > TIMEOUT_MAX_MS) return reply_config(buf, cap, R_BAD_ARG, "timeout must be 50..600000 ms");
        config.write_timeout_ms = v; break;
    case KEY_ECC_LEN:
        if (v > 64) return reply_config(buf, cap, R_BAD_ARG, "ecc_len must be 0..64");
        config.ecc_len = (uint8_t)v; break;
    case KEY_NO_RETRY:   config.no_retry = (v != 0); break;
    case KEY_DEVICE:
        if (v > 1) return reply_config(buf, cap, R_BAD_ARG, "device must be 0 (master) or 1 (slave)");
        if (is_mounted) return reply_config(buf, cap, R_MOUNTED, "unmount before changing device");
        config.dev_base = v ? 0xB0 : 0xA0;
        ide_select_device(config.dev_base);
        drive_clear_identity();
        break;
    default:
        return reply_config(buf, cap, R_BAD_ARG, "unknown config key");
    }
    drive.api_gen++;
    return reply_config(buf, cap, R_OK, NULL);
}

static int32_t op_detect(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint8_t dev = a[0] == 1 ? 0xA0 : a[0] == 2 ? 0xB0 : 0;
    if (a[0] > 2) return reply_msg(buf, cap, R_BAD_ARG, "device must be 0 (auto), 1 (master) or 2 (slave)");
    uint8_t geo = a[1];
    switch (geo) {
    case 0: geo = GEO_AUTO; break;
    case 1: geo = GEO_NORMAL; break;
    case 2: geo = GEO_LARGE; break;
    case 3: geo = GEO_LBA; break;
    case 4: geo = GEO_MANUAL; break;
    case 5: geo = GEO_KEEP; break;
    default: return reply_msg(buf, cap, R_BAD_ARG, "geometry must be 0..5");
    }
    int r = drive_detect(dev, geo, be16(&a[2]), a[4], a[5], a[6]);
    drive.api_gen++;
    const char *msg = NULL;
    if (r == R_MOUNTED) msg = "unmount first";
    else if (r == R_NO_DRIVE) msg = "no device answered on the selected position(s)";
    else if (r == R_IDENTIFY_FAILED) msg = "device present but IDENTIFY failed; retry, or use force with manual geometry";
    else if (r == R_UNSUPPORTED) msg = "drive does not support LBA";
    else if (r == R_BAD_ARG) msg = "invalid manual geometry (heads 1..16, cyls/spt > 0)";
    return reply_status(buf, cap, r, msg);
}

static int32_t op_geometry(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    if (is_mounted) return reply_status(buf, cap, R_MOUNTED, "unmount first");
    static const uint8_t map[] = {0, GEO_NORMAL, GEO_LARGE, GEO_LBA, GEO_MANUAL};
    if (a[1] < 1 || a[1] > 4) return reply_msg(buf, cap, R_BAD_ARG, "geometry must be 1..4");
    int r = drive_apply_geometry(map[a[1]], be16(&a[2]), a[4], a[5]);
    drive.api_gen++;
    return reply_status(buf, cap, r, r == R_IDENTIFY_FAILED ? "no IDENTIFY data; detect first or use manual" : NULL);
}

static int32_t op_reset(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    if (a[0] == 1) ide_reset_drive();
    else ide_soft_reset();
    drive.api_gen++;
    uint8_t st = ide_read_alt_status();
    return reply_status(buf, cap, (st & ATA_ST_BSY) ? R_NOT_READY : R_OK,
                        (st & ATA_ST_BSY) ? "drive still busy after reset" : NULL);
}

static int32_t op_identify(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    if (cap < HDR_LEN + 512) return reply_msg(buf, cap, R_ALLOC_TOO_SMALL, "need 528 bytes");
    if (a[0] & 1) {
        if (!drive.identified) return reply_msg(buf, cap, R_IDENTIFY_FAILED, "no cached IDENTIFY data");
        memcpy(buf + HDR_LEN, drive.id, 512);
        return finish(buf, R_OK, CT_BINARY, 512);
    }
    ide_cmd_t c = {0};
    c.command = 0xEC;
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = buf + HDR_LEN;
    c.sectors = 1;
    c.flags = IDE_F_NEED_DRDY;
    c.ready_ms = 1000;
    c.timeout_ms = 5000;
    int r = ide_exec(&c);
    drive_record(LOGOP_IDENTIFY, &c, 0, 1);
    if (r != IDE_OK) return reply_msg(buf, cap, drive_result_from_ide(r), "IDENTIFY DEVICE failed");
    drive_note_identify((const uint16_t *)(buf + HDR_LEN));
    drive.api_gen++;
    return finish(buf, R_OK, CT_BINARY, 512);
}

static int32_t op_taskfile(uint8_t *buf, uint32_t cap) {
    uint8_t tf[8];
    ide_lock();
    uint8_t alt = ide_read_alt_status();
    ide_read_taskfile(tf);
    ide_unlock();
    char sb[48], eb[48];
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN);
    j_obj(&j, NULL);
    j_str(&j, "result", "ok");
    j_bool(&j, "valid", !(alt & ATA_ST_BSY));
    j_uint(&j, "error", tf[1]);
    j_str(&j, "error_bits", ata_error_bits(tf[1], eb));
    j_uint(&j, "count", tf[2]);
    j_uint(&j, "lba_lo", tf[3]);
    j_uint(&j, "lba_mid", tf[4]);
    j_uint(&j, "lba_hi", tf[5]);
    j_uint(&j, "device", tf[6]);
    j_uint(&j, "status", tf[7]);
    j_str(&j, "status_bits", ata_status_bits(tf[7], sb));
    j_uint(&j, "alt_status", alt);
    j_end(&j);
    return finish(buf, R_OK, CT_JSON, j.len);
}

// READ: up to max_read_sectors with full diagnostics; optional read-through-error.
static int32_t op_read(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint32_t lba = be32(&a[0]);
    uint16_t count = be16(&a[4]);
    uint8_t flags = a[6];
    uint32_t tmo = be32(&a[8]);
    uint32_t max = (cap - HDR_LEN - RES_LEN) / 512;
    if (count == 0 || count > max) return reply_msg(buf, cap, R_BAD_ARG, "count must be 1..max_read_sectors");
    if (tmo && (tmo < TIMEOUT_MIN_MS || tmo > TIMEOUT_MAX_MS)) return reply_msg(buf, cap, R_BAD_ARG, "timeout must be 0 or 50..600000 ms");
    if (!drive_geometry_valid()) return reply_msg(buf, cap, R_NO_GEOMETRY, "detect or set geometry first");
    if ((uint64_t)lba + count > drive_total_sectors()) return reply_msg(buf, cap, R_BAD_ARG, "beyond end of drive");

    ide_cmd_t c = {0};
    if (!ide_cmd_address(&c, lba, count, true)) return reply_msg(buf, cap, R_BAD_ARG, "address not expressible");
    bool no_retry = (flags & 1) || config.no_retry;
    c.command = c.ext ? 0x24 : (no_retry ? 0x21 : 0x20);
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = buf + HDR_LEN + RES_LEN;
    c.sectors = count;
    c.flags = IDE_F_NEED_DRDY | ((flags & 2) ? IDE_F_TAKE_ERR_DATA : 0);
    c.timeout_ms = tmo ? tmo : config.read_timeout_ms;
    drive_stats.read_cmds++;
    int r = ide_exec(&c);
    drive_stats.read_sectors += c.done;
    if (r != IDE_OK) drive_stats.read_errors++;
    drive_record(LOGOP_READ_DIAG, &c, lba, count);

    put_result(buf + HDR_LEN, &c, lba, count, false);
    uint32_t nblk = c.done + (c.err_data ? 1 : 0);
    return finish(buf, drive_result_from_ide(r), CT_BINARY, RES_LEN + nblk * 512);
}

static int32_t op_read_long(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint32_t lba = be32(&a[0]);
    uint8_t ecc = a[4] ? a[4] : drive_ecc_len();
    uint8_t flags = a[6];
    uint32_t tmo = be32(&a[8]);
    if (ecc > 64) return reply_msg(buf, cap, R_BAD_ARG, "ecc_len must be 0..64");
    if (cap < (uint32_t)(HDR_LEN + RES_LEN + 512 + ecc)) return reply_msg(buf, cap, R_ALLOC_TOO_SMALL, "allocation too small");
    if (tmo && (tmo < TIMEOUT_MIN_MS || tmo > TIMEOUT_MAX_MS)) return reply_msg(buf, cap, R_BAD_ARG, "timeout must be 0 or 50..600000 ms");
    if (!drive_geometry_valid()) return reply_msg(buf, cap, R_NO_GEOMETRY, "detect or set geometry first");
    if ((uint64_t)lba >= drive_total_sectors()) return reply_msg(buf, cap, R_BAD_ARG, "beyond end of drive");

    ide_cmd_t c = {0};
    if (!ide_cmd_address(&c, lba, 1, false)) return reply_msg(buf, cap, R_BAD_ARG, "READ LONG needs a CHS/LBA28 address");
    c.command = (flags & 1) ? 0x23 : 0x22;
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = buf + HDR_LEN + RES_LEN;
    memset(c.buf, 0, 512 + ecc);
    c.sectors = 1;
    c.ecc_len = ecc;
    c.flags = IDE_F_NEED_DRDY | IDE_F_TAKE_ERR_DATA;
    c.timeout_ms = tmo ? tmo : config.read_timeout_ms;
    int r = ide_exec(&c);
    drive_record(LOGOP_READ_LONG, &c, lba, 1);

    put_result(buf + HDR_LEN, &c, lba, 1, false);
    bool data = c.done == 1 || c.err_data;
    // A protocol result here usually means the ECC length was wrong (DRQ left over).
    return finish(buf, drive_result_from_ide(r), CT_BINARY, RES_LEN + (data ? 512u + ecc : 0u));
}

static int32_t op_verify(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint32_t lba = be32(&a[0]);
    uint32_t count = be32(&a[4]);
    uint32_t tmo = be32(&a[8]);
    if (count == 0) return reply_msg(buf, cap, R_BAD_ARG, "count must be > 0");
    if (tmo && (tmo < TIMEOUT_MIN_MS || tmo > TIMEOUT_MAX_MS)) return reply_msg(buf, cap, R_BAD_ARG, "timeout must be 0 or 50..600000 ms");
    if (!drive_geometry_valid()) return reply_msg(buf, cap, R_NO_GEOMETRY, "detect or set geometry first");
    if ((uint64_t)lba + count > drive_total_sectors()) return reply_msg(buf, cap, R_BAD_ARG, "beyond end of drive");

    uint32_t t0 = time_us_32();
    uint32_t done = 0;
    ide_cmd_t c = {0};
    int r = IDE_OK;
    bool partial = false;
    while (done < count) {
        if (time_us_32() - t0 > VERIFY_BUDGET_US) { partial = true; break; }
        uint32_t n = count - done;
        if (n > 256) n = 256;
        memset(&c, 0, sizeof(c));
        if (!ide_cmd_address(&c, lba + done, n, true)) { r = IDE_PROTOCOL; break; }
        c.command = c.ext ? 0x42 : (config.no_retry ? 0x41 : 0x40);
        c.proto = IDE_PROTO_NONDATA;
        c.flags = IDE_F_NEED_DRDY;
        c.timeout_ms = tmo ? tmo : (config.read_timeout_ms * 2 > 30000 ? config.read_timeout_ms * 2 : 30000);
        r = ide_exec(&c);
        drive_record(LOGOP_VERIFY, &c, lba + done, n);
        if (r != IDE_OK) {
            uint32_t fl;
            if (ide_cmd_result_lba(&c, &fl) && fl >= lba + done && fl < lba + done + n) done = fl - lba;
            break;
        }
        done += n;
    }
    c.done = done;
    c.elapsed_us = time_us_32() - t0;
    put_result(buf + HDR_LEN, &c, lba, count > 0xFFFF ? 0xFFFF : count, partial);
    put_be32(buf + HDR_LEN + 28, done);           // full 32-bit verified count
    put_be32(buf + HDR_LEN + 12, c.elapsed_us);
    if (r != IDE_OK) {
        uint32_t fl = lba + done;
        put_be32(buf + HDR_LEN + 16, fl);
    }
    return finish(buf, drive_result_from_ide(r), CT_BINARY, RES_LEN);
}

// CORRUPT: make a deliberately damaged sector for testing recovery tools.
// READ LONG the sector, flip 'bits' data bits, WRITE LONG it back with the
// original ECC bytes, so the stored ECC no longer matches the data.  Few bits
// are usually corrected by the drive; more give UNC.  Rewriting the sector
// normally repairs it.  Needs write protect off and an ARM (consumed).
static int32_t op_corrupt(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint32_t lba = be32(&a[0]);
    uint8_t bits = a[4];
    uint8_t ecc = a[5] ? a[5] : drive_ecc_len();
    bool armed_ok = api_take_arm();
    if (config.drive_write_protected) return reply_msg(buf, cap, R_WRITE_PROTECTED, "turn write protect off first");
    if (!armed_ok) return reply_msg(buf, cap, R_NOT_ARMED, "ARM first (destructive)");
    if (bits == 0 || ecc > 64) return reply_msg(buf, cap, R_BAD_ARG, "bits must be 1..255, ecc_len 0..64");
    if (!drive_geometry_valid()) return reply_msg(buf, cap, R_NO_GEOMETRY, "detect or set geometry first");
    if ((uint64_t)lba >= drive_total_sectors()) return reply_msg(buf, cap, R_BAD_ARG, "beyond end of drive");

    static uint8_t sec[512 + 64];
    ide_cmd_t c = {0};
    if (!ide_cmd_address(&c, lba, 1, false)) return reply_msg(buf, cap, R_BAD_ARG, "needs a CHS/LBA28 address");
    c.command = 0x22;
    c.proto = IDE_PROTO_PIO_IN;
    c.buf = sec;
    c.sectors = 1;
    c.ecc_len = ecc;
    c.flags = IDE_F_NEED_DRDY | IDE_F_TAKE_ERR_DATA;
    c.timeout_ms = config.read_timeout_ms;
    int r = ide_exec(&c);
    drive_record(LOGOP_READ_LONG, &c, lba, 1);
    if (!(c.done == 1 || c.err_data)) {
        put_result(buf + HDR_LEN, &c, lba, 1, false);
        return finish(buf, drive_result_from_ide(r), CT_BINARY, RES_LEN);
    }
    for (uint32_t i = 0; i < bits; i++) {          // spread the flips over the sector
        uint32_t bit = (i * 1031u + 7u) % 4096u;
        sec[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
    }
    ide_cmd_t w = {0};
    ide_cmd_address(&w, lba, 1, false);
    w.command = 0x32;                              // WRITE LONG (with retries)
    w.proto = IDE_PROTO_PIO_OUT;
    w.buf = sec;
    w.sectors = 1;
    w.ecc_len = ecc;
    w.flags = IDE_F_NEED_DRDY;
    w.timeout_ms = config.write_timeout_ms;
    r = ide_exec(&w);
    drive_record(LOGOP_WRITE_LONG, &w, lba, 1);
    w.done = (r == IDE_OK) ? 1 : 0;
    put_result(buf + HDR_LEN, &w, lba, 1, false);
    return finish(buf, drive_result_from_ide(r), CT_BINARY, RES_LEN);
}

static int32_t op_log(uint8_t *buf, uint32_t cap, const uint8_t *a) {
    uint32_t start = be32(&a[0]);
    uint32_t n = drive_log_count();
    json_t j;
    j_init(&j, (char *)buf + HDR_LEN, cap - HDR_LEN - 48);   // keep room to close cleanly
    j_obj(&j, NULL);
    j_str(&j, "result", "ok");
    j_uint(&j, "total", drive_log_total());
    j_uint(&j, "retained", n);
    j_uint(&j, "start", start);
    j_arr(&j, "entries");
    uint32_t i = start;
    for (; i < n; i++) {
        json_mark_t m = j_mark(&j);
        json_log_entry(&j, NULL, drive_log_get(i));
        if (j.overflow) { j_restore(&j, m); break; }
    }
    j_end(&j);
    j.cap += 48;
    j_uint(&j, "returned", i - start);
    if (i < n) j_uint(&j, "next", i); else j_null(&j, "next");
    j_end(&j);
    return finish(buf, R_OK, CT_JSON, j.len);
}

// ---------------------------------------------------------------------------
//  Dispatcher
// ---------------------------------------------------------------------------

int32_t api_handle(uint8_t lun, const uint8_t *cdb, uint8_t *buf, uint32_t cap) {
    if (cdb[2] != 'A' || cdb[3] != 'B') {
        sense_set(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);   // not an ATAboy API CDB
        return -1;
    }
    if (cap < 256) {
        sense_set(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);   // allocation length too small
        return -1;
    }
    cur_op = cdb[1];
    const uint8_t *a = &cdb[4];
    drive_stats.api_cmds++;

    switch (cur_op) {
    case OP_INFO:       return op_info(buf, cap);
    case OP_STATUS:     return reply_status(buf, cap, R_OK, NULL);
    case OP_CONFIG_GET: return reply_config(buf, cap, R_OK, NULL);
    case OP_CONFIG_SET: return op_config_set(buf, cap, a);
    case OP_CONFIG_SAVE:
        config_save();
        return reply_config(buf, cap, R_OK, "saved to flash");
    case OP_CONFIG_DEFAULTS: {
        if (is_mounted) return reply_config(buf, cap, R_MOUNTED, "unmount first");
        config_defaults();
        ide_select_device(config.dev_base);
        ide_set_iordy(config.iordy_enabled);
        drive.api_gen++;
        return reply_config(buf, cap, R_OK, "defaults loaded (not saved)");
    }
    case OP_DETECT:     return op_detect(buf, cap, a);
    case OP_GEOMETRY:   return op_geometry(buf, cap, a);
    case OP_MOUNT: {
        int r = drive_mount();
        drive.api_gen++;
        return reply_status(buf, cap, r, r == R_NO_GEOMETRY ? "detect or set geometry first" : NULL);
    }
    case OP_UNMOUNT:
        drive_unmount();
        drive.api_gen++;
        return reply_status(buf, cap, R_OK, NULL);
    case OP_RESET:      return op_reset(buf, cap, a);
    case OP_IDENTIFY:   return op_identify(buf, cap, a);
    case OP_TASKFILE:   return op_taskfile(buf, cap);
    case OP_READ:       return op_read(buf, cap, a);
    case OP_READ_LONG:  return op_read_long(buf, cap, a);
    case OP_VERIFY:     return op_verify(buf, cap, a);
    case OP_CORRUPT:    return op_corrupt(buf, cap, a);
    case OP_LOG:        return op_log(buf, cap, a);
    case OP_LOG_CLEAR:
        if (a[0] & 1) drive_log_clear();
        if (a[0] & 2) drive_stats_clear();
        return reply_msg(buf, cap, R_OK, NULL);
    case OP_ARM:
        if (be32(&a[0]) != 0x41524D21u) return reply_msg(buf, cap, R_BAD_ARG, "bad arm magic");   // "ARM!"
        armed = true;
        armed_until_ms = now_ms() + ARM_WINDOW_MS;
        return reply_msg(buf, cap, R_OK, "armed for one dangerous pass-through command within 10 s");
    case OP_REBOOT:
        if (be32(&a[4]) != 0x424F4F54u || a[0] > 1) return reply_msg(buf, cap, R_BAD_ARG, "bad reboot magic/mode");  // "BOOT"
        reboot_mode = a[0];
        reboot_at_ms = now_ms() + 200;    // let the reply and CSW go out first
        return reply_msg(buf, cap, R_OK, a[0] ? "rebooting to BOOTSEL" : "rebooting");
    default:
        return reply_msg(buf, cap, R_BAD_SUBOP, "unknown sub-op");
    }
}
