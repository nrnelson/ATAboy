#ifndef ATABOY_H
#define ATABOY_H

// libataboy: talk to an ATAboy running the automation-API firmware.
//
// Transports: the "ATAboy API" USB serial port (any user with access to the
// tty: no root, no admin, Linux/macOS/Windows), or SG_IO on Linux (root).
// Protocol: docs/API.md; the constants and framing are the firmware's own
// (firmware/api_proto.h, firmware/apiframe.h), linked in, not re-implemented.
//
// Conventions
//  - Functions return AB_OK (0) or a negative ab_err; ab_errmsg(dev) (or
//    ab_last_error() before a device exists) says what went wrong.
//  - JSON results are cJSON trees owned by the caller (cJSON_Delete).
//  - A device-level failure (bad sector, refused op) is not an error of the
//    call: the reply comes back with AB_ERR_API and its JSON, whose "result"
//    names what happened.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATABOY_LIB_VERSION "2.0.0"

typedef enum {
    AB_OK            =  0,
    AB_ERR_API       = -1,   // the device answered with a result other than ok
    AB_ERR_TRANSPORT = -2,   // I/O failure, no reply, malformed reply
    AB_ERR_NODEV     = -3,   // no ATAboy found / not this one
    AB_ERR_PERM      = -4,   // no access (tty group, root for SG_IO)
    AB_ERR_ARG       = -5,   // bad argument from the caller
    AB_ERR_CHECK     = -6,   // CHECK CONDITION (malformed request): ab_sense()
    AB_ERR_NOMEM     = -7,
    AB_ERR_IO        = -8,   // local file I/O
    AB_ERR_UNSUPPORTED = -9, // not available on this platform/transport/firmware
} ab_err;

// ---------------------------------------------------------------------------
//  Discovery
// ---------------------------------------------------------------------------

typedef struct {
    char     tty[256];          // API serial port ("" if none): /dev/ttyACM1, COM5, /dev/cu.usbmodem...
    char     block[64];         // USB disk (Linux /dev/sdX, Windows \\.\PhysicalDriveN), "" if none
    char     sg[64];            // Linux SCSI generic node, "" if none
    char     serial[64];        // USB serial number (RP2350 board id)
    char     rev[16];           // SCSI INQUIRY revision ("api1"), "" if unknown
    uint64_t size_sectors;      // capacity the host sees (0 = no medium)
    int      scsi_timeout_s;    // Linux SCSI command timeout, 0 if unknown
    char     sysfs[512];        // Linux: SCSI device sysfs dir ("" elsewhere)
    char     usb[512];          // Linux: USB device sysfs dir
} ab_devinfo;

// Fills up to max entries; returns how many ATAboys are attached (may be > max).
int ab_find_devices(ab_devinfo *out, int max);

// ---------------------------------------------------------------------------
//  Connection
// ---------------------------------------------------------------------------

typedef struct ab_dev ab_dev;

// path: NULL (first device, or the one with USB serial 'serial'), a serial
// port, or (Linux) /dev/sgN or /dev/sdX.  transport: NULL/"auto" (serial
// port if usable, else SG_IO), "serial" or "sgio".
int  ab_open(ab_dev **out, const char *path, const char *serial, const char *transport);
void ab_close(ab_dev *dev);

const char *ab_errmsg(const ab_dev *dev);     // message for the last failed call on dev
const char *ab_last_error(void);              // same, for ab_open/ab_find_devices failures
const char *ab_err_name(int err);
const ab_devinfo *ab_dev_info(const ab_dev *dev);   // NULL if not matched to a discovered device
const char *ab_transport_name(const ab_dev *dev);   // "serial" or "sgio"
void ab_sense(const ab_dev *dev, uint8_t *key, uint8_t *asc, uint8_t *ascq);

// Boot id tracking: a reboot between calls (also across processes, if the
// caller seeds the last known id) is reported through this callback and in
// ab_link_info().
typedef void (*ab_reboot_cb)(void *ctx, uint32_t old_boot_id, uint32_t new_boot_id, uint8_t op);
void ab_on_reboot(ab_dev *dev, ab_reboot_cb cb, void *ctx);
void ab_seed_boot_id(ab_dev *dev, uint32_t boot_id);
bool ab_boot_id(const ab_dev *dev, uint32_t *boot_id);        // false until an extended reply came
const char *ab_dev_key(const ab_dev *dev);                    // USB serial, else path
cJSON *ab_link_info(const ab_dev *dev);   // transport, path, serial, boot id, state counter, reboots

// ---------------------------------------------------------------------------
//  Raw API call
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  proto, op, result, ctype, flags;
    uint32_t uptime_ms;
    bool     ext;                       // 32-byte extended header
    uint32_t boot_id, state_counter, request_id;
    bool     replayed, rid_taken, rebooted;
    const uint8_t *payload;             // into raw
    size_t   payload_len;
    cJSON   *json;                      // parsed JSON payload (ctype JSON), else NULL
    uint8_t  raw[4096];
    size_t   raw_len;
} ab_reply;

// One request: op + up to 12 argument bytes (CDB 4-15).  Uses the extended
// header when the firmware has it; state-changing ops get a fresh run-once
// request id, so a lost reply can be re-sent safely.  r->json must be freed
// with ab_reply_free.  Returns AB_OK even when r->result != 0.
int  ab_call(ab_dev *dev, uint8_t op, const uint8_t *args, size_t nargs, uint32_t alloc,
             uint32_t timeout_ms, ab_reply *r);
void ab_reply_free(ab_reply *r);
const char *ab_result_name(int result);

// ---------------------------------------------------------------------------
//  Operations returning JSON (*out set when the device sent JSON, even on
//  AB_ERR_API)
// ---------------------------------------------------------------------------

int ab_info(ab_dev *dev, cJSON **out);
int ab_status(ab_dev *dev, cJSON **out);
int ab_config_get(ab_dev *dev, cJSON **out);                  // the "config" object
// key: write_protect, auto_mount, iordy, intrq, read_timeout_ms,
// write_timeout_ms, ecc_len, no_retry, device; value as text (on/off/1/0/
// true/false, numbers, master/slave).  *out = new config object.
int ab_config_set(ab_dev *dev, const char *key, const char *value, cJSON **out);
int ab_config_save(ab_dev *dev, cJSON **out);
int ab_config_defaults(ab_dev *dev, cJSON **out);
extern const char *const ab_config_keys[];                    // NULL-terminated

typedef struct {
    const char *device;      // "auto" (default), "master", "slave"
    const char *geometry;    // "auto" (default), normal, large, lba, manual, keep
    unsigned cyls, heads, spt;   // manual
    bool no_probe, force, mount, force_mount;
} ab_detect_opts;
int ab_detect(ab_dev *dev, const ab_detect_opts *o, cJSON **out);
int ab_geometry(ab_dev *dev, const char *mode, unsigned cyls, unsigned heads, unsigned spt, cJSON **out);
int ab_mount(ab_dev *dev, bool force, cJSON **out);
int ab_unmount(ab_dev *dev, cJSON **out);
int ab_reset(ab_dev *dev, bool hard, cJSON **out);
int ab_park(ab_dev *dev, int cylinder /* -1 = no seek */, cJSON **out);
int ab_taskfile(ab_dev *dev, cJSON **out);
int ab_log(ab_dev *dev, cJSON **out);                         // all retained entries
int ab_clear(ab_dev *dev, bool log, bool stats, cJSON **out);
int ab_arm(ab_dev *dev, cJSON **out);
int ab_reboot(ab_dev *dev, int mode /* 0 restart, 1 BOOTSEL, 2 fault test */, cJSON **out);

// ---------------------------------------------------------------------------
//  Binary operations
// ---------------------------------------------------------------------------

int ab_identify(ab_dev *dev, bool cached, uint8_t raw[512]);
cJSON *ab_decode_identify(const uint8_t raw[512]);

// Decoded binary result block (+ data) of READ, READ_LONG, VERIFY, CORRUPT.
typedef struct {
    int      result;                    // API result code
    int      ide_result;                // APIIDE_*
    uint32_t lba;
    uint16_t count;
    uint32_t done;                      // VERIFY: 32-bit count
    uint8_t  status, error, command, flags, ecc_len;
    uint32_t elapsed_us;
    bool     has_fail_lba;
    uint32_t fail_lba;
    uint8_t  tf[5];
    const uint8_t *data;                // into the reply
    size_t   data_len;
} ab_ataresult;
cJSON *ab_ataresult_json(const ab_ataresult *a);

// When the device answers with JSON instead of a result block (bad argument,
// no geometry), these return AB_ERR_API with *json_out set (if non-NULL).
// The ab_reply holds the data: keep it until done with a->data, then free.
int ab_read(ab_dev *dev, uint32_t lba, unsigned count, bool no_retry, bool through_error,
            uint32_t timeout_ms, ab_ataresult *a, ab_reply *r, cJSON **json_out);
int ab_read_long(ab_dev *dev, uint32_t lba, unsigned ecc_len, bool no_retry, uint32_t timeout_ms,
                 ab_ataresult *a, ab_reply *r, cJSON **json_out);
int ab_verify(ab_dev *dev, uint32_t lba, uint32_t count, uint32_t timeout_ms,
              ab_ataresult *a, ab_reply *r, cJSON **json_out);
int ab_corrupt(ab_dev *dev, uint32_t lba, unsigned bits, unsigned ecc_len,
               ab_ataresult *a, ab_reply *r, cJSON **json_out);

// ---------------------------------------------------------------------------
//  Multi-step operations (shared by the CLI and the MCP server)
// ---------------------------------------------------------------------------

typedef void (*ab_progress_cb)(void *ctx, const char *line);

typedef struct {
    const char *out_path;        // write data here (NULL: don't)
    bool        image;           // out_path is a full-disk image: write at LBA*512, in place
    bool        no_retry, through_error, keep_going;
    uint32_t    timeout_ms;      // per sector, 0 = config
    double      time_budget_s;   // 0 = none
    bool        keep_data;       // return the data in *data / *data_len (caller frees)
    uint8_t   **data;
    size_t     *data_len;
} ab_read_range_opts;

// Read [lba, lba+count) through the API in max-size chunks with per-chunk
// diagnostics; after an error, sector by sector.  *out: {"result", "start",
// "count", "ok_sectors", "bad_lbas", "next_lba", "seconds", "chunks"}.
// Returns AB_OK (also when sectors failed: see "result"), else an error.
int ab_read_range(ab_dev *dev, uint32_t lba, uint32_t count, const ab_read_range_opts *o, cJSON **out);

// READ VERIFY scan of [lba, lba+count); bad sectors skipped one at a time.
// *out: {"result", "start", "count", "scanned", "next_lba", "complete",
// "bad", "bad_lbas", "seconds", "requests"}.
int ab_scan(ab_dev *dev, uint32_t lba, uint32_t count, uint32_t timeout_ms, unsigned max_errors,
            double time_budget_s, ab_progress_cb progress, void *ctx, cJSON **out);

// READ LONG 'votes' times and majority-vote every byte.  payload must hold
// 512 + 255 bytes; *payload_len = 0 when no read returned data.  *info: the
// last read's result dict plus "vote" {"reads", "unstable_bytes"} and
// "ecc_hex".
int ab_read_long_vote(ab_dev *dev, uint32_t lba, unsigned votes, unsigned ecc_len, bool no_retry,
                      uint32_t timeout_ms, uint8_t *payload, size_t *payload_len, cJSON **info);

// ---------------------------------------------------------------------------
//  Raw ATA command: SAT ATA PASS-THROUGH (16) over Linux SG_IO (root).  Uses
//  the ATAboy's SCSI node even when the device was opened on the serial
//  port.  AB_ERR_UNSUPPORTED on Windows and macOS.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t        command, feature, count, lba, device;  // packed modulo each register's width
    bool           ext;             // 48-bit command
    uint32_t       data_in;         // PIO data-in bytes (whole sectors), 0 = none
    const uint8_t *data_out;        // PIO data-out (NULL = none; takes precedence over data_in)
    size_t         data_out_len;
    bool           ck_cond;         // ask for the result registers
    uint32_t       timeout_ms;      // 0 = 60000
} ab_ata_cmd;

// The 16-byte CDB for c.
void ab_ata_cdb(const ab_ata_cmd *c, uint8_t cdb[16]);
// Registers from the ATA Status Return descriptor in descriptor-format sense
// data: {"status", "status_bits", "error", "error_bits", "count", "lba",
// "device"}; NULL if there is none.
cJSON *ab_parse_ata_return(const uint8_t *sense, size_t len);
// *out: {"command", "ok", ["sense", "sense_key", registers | "blocked"]}.
// AB_OK whenever the command reached the device (see "ok").  Data-in comes
// back in *data (malloc'd, caller frees) when data is non-NULL.
int ab_ata(ab_dev *dev, const ab_ata_cmd *c, cJSON **out, uint8_t **data, size_t *data_len);

// ---------------------------------------------------------------------------
//  JSON output (the CLI's format: Python json.dumps(indent=2) compatible)
// ---------------------------------------------------------------------------

// Accessors that tolerate missing keys and wrong types (o may be NULL).
cJSON      *ab_jobj_get(const cJSON *o, const char *key);
bool        ab_jbool(const cJSON *o, const char *key, bool dflt);
double      ab_jnum(const cJSON *o, const char *key, double dflt);
const char *ab_jstr(const cJSON *o, const char *key, const char *dflt);

// Returns a malloc'd string; indent 0 = compact one line.
char *ab_json_print(const cJSON *j, int indent);
// Status/error register names "BSY|DRDY|..." (buf >= 64 bytes).
const char *ab_status_bits(uint8_t st, char *buf);
const char *ab_error_bits(uint8_t er, char *buf);

#ifdef __cplusplus
}
#endif
#endif
