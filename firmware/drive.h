#ifndef DRIVE_H
#define DRIVE_H

// Drive state shared by the USB side (core 0) and the menu (core 1), plus the
// high-level operations both of them use: detect, geometry, mount, logged
// sector I/O.  Geometry itself lives in config_t.

#include <stdint.h>
#include <stdbool.h>
#include "ide.h"

typedef struct {
    bool     present;          // last probe found a device
    bool     identified;       // id[] holds valid IDENTIFY data
    bool     forced;           // geometry entered without IDENTIFY data
    uint16_t id[256];
    char     model[41];
    char     serial[21];
    char     fw[9];
    volatile uint32_t api_gen; // bumped when the API changes state the menu shows
} drive_state_t;

extern drive_state_t drive;
extern volatile bool is_mounted;
extern volatile bool media_changed_waiting;

// Operation codes for the error log
#define LOGOP_READ      1
#define LOGOP_WRITE     2
#define LOGOP_READ_LONG 3
#define LOGOP_VERIFY    4
#define LOGOP_PASSTHRU  5
#define LOGOP_IDENTIFY  6
#define LOGOP_READ_DIAG 7

typedef struct {
    uint32_t seq;
    uint32_t t_ms;            // ms since boot
    uint8_t  op;              // OP_*
    uint8_t  cmd;             // ATA command
    int8_t   result;          // IDE_* result
    uint8_t  status, error;
    bool     tf_valid;
    bool     err_data;
    bool     recovered;
    uint32_t lba;             // first LBA of the request
    uint32_t fail_lba;        // where the drive stopped (from registers if valid)
    uint16_t count;           // sectors requested
    uint16_t done;            // sectors transferred before the failure
    uint32_t elapsed_ms;
    uint32_t timeout_ms;
    uint8_t  tf[5];           // count, lba_lo/sector, lba_mid/cyl_lo, lba_hi/cyl_hi, device (valid if tf_valid)
} errlog_entry_t;

#define ERRLOG_SIZE 64

typedef struct {
    uint32_t read_cmds, read_sectors, read_errors;
    uint32_t write_cmds, write_sectors, write_errors;
    uint32_t timeouts, recoveries;
    uint32_t passthru_cmds, passthru_errors, passthru_blocked;
    uint32_t api_cmds;
} drive_stats_t;

extern drive_stats_t drive_stats;

// --- result codes shared with the API (docs/API.md "Result codes") ---
#define R_OK               0
#define R_BAD_SUBOP        1
#define R_BAD_ARG          2
#define R_NO_DRIVE         3
#define R_NOT_MOUNTED      4
#define R_WRITE_PROTECTED  5
#define R_NOT_ARMED        6
#define R_ATA_ERROR        7
#define R_TIMEOUT          8
#define R_NOT_READY        9
#define R_BUS_FAULT       10
#define R_NO_GEOMETRY     11
#define R_MOUNTED         12
#define R_IDENTIFY_FAILED 13
#define R_UNSUPPORTED     14
#define R_PROTOCOL        15
#define R_ALLOC_TOO_SMALL 16

int      drive_result_from_ide(int ide_result);

// State helpers
void     drive_note_identify(const uint16_t *id);   // also used by the menu
void     drive_note_forced(void);
void     drive_clear_identity(void);
bool     drive_geometry_valid(void);
uint64_t drive_total_sectors(void);                 // of the configured geometry
uint8_t  drive_ecc_len(void);                       // effective READ LONG ECC length

// High-level operations (return R_*)
#define DETECT_SKIP_PROBE 0x01   // no reset/probe: select + IDENTIFY only
#define DETECT_MOUNT      0x02   // mount on success
#define DETECT_FORCE      0x04   // accept MANUAL geometry if IDENTIFY fails
#define GEO_AUTO        0xFE     // LBA if supported, else NORMAL
#define GEO_KEEP        0xFF     // don't touch geometry

int      drive_detect(uint8_t only_dev, uint8_t geo_mode, uint16_t c, uint8_t h, uint8_t s, uint8_t flags);
int      drive_apply_geometry(uint8_t mode, uint16_t c, uint8_t h, uint8_t s);
int      drive_mount(void);
int      drive_unmount(void);

// Logged sector I/O for the mass-storage path.  Return bytes or -1.
int32_t  drive_read(uint32_t lba, uint32_t count, uint8_t *buf);
int32_t  drive_write(uint32_t lba, uint32_t count, const uint8_t *buf);

// Record a finished command in stats and (if it failed) the error log.
void     drive_record(uint8_t op, const ide_cmd_t *c, uint32_t lba, uint32_t count);

// Error log access
uint32_t drive_log_total(void);                     // failures logged since boot/clear
uint32_t drive_log_count(void);                     // entries retained (<= ERRLOG_SIZE)
const errlog_entry_t *drive_log_get(uint32_t i);    // 0 = oldest retained
const errlog_entry_t *drive_last_failure(void);     // most recent (NULL if none)
void     drive_log_clear(void);
void     drive_stats_clear(void);

#endif
