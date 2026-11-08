#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_MAGIC     0x1DE45707
#define CONFIG_EXT_MAGIC 0xA7AB0E01   // extension block below is valid
#define CONFIG_EXT2_MAGIC 0xA7AB0E02  // extension block 2 + CRC are valid

// Geometry modes (config.geo_mode)
#define GEO_NONE    0
#define GEO_NORMAL  1   // IDENTIFY default CHS
#define GEO_LARGE   2   // translated CHS (heads raised until cyls <= 65535)
#define GEO_LBA     3   // LBA28 / LBA48 from IDENTIFY
#define GEO_MANUAL  4   // user-supplied CHS

// Read/write timeout limits (ms), applied to both menu and API settings.
#define TIMEOUT_MIN_MS   50
#define TIMEOUT_MAX_MS   600000

typedef struct {
    uint32_t magic;
    uint8_t  main_selected;
    uint8_t  feat_selected;
    bool     drive_write_protected;
    bool     auto_mount;
    bool     iordy_enabled;
    bool     intrq_enabled;
    bool     use_lba_mode;
    uint16_t cyls;
    uint8_t  heads;
    uint8_t  spt;
    uint64_t lba_sectors;
    uint8_t  dev_base;            // 0xA0 = master, 0xB0 = slave

    // --- Extension block.  Appended so settings saved by stock firmware
    // still load; ext_magic tells whether the fields below were ever saved.
    uint32_t ext_magic;
    uint32_t read_timeout_ms;     // per-sector DRQ wait for reads
    uint32_t write_timeout_ms;    // per-sector DRQ / completion wait for writes
    uint8_t  ecc_len;             // READ LONG ECC bytes, 0 = auto (IDENTIFY word 22, else 4)
    bool     no_retry;            // use READ SECTORS without retries (0x21) for CHS/LBA28
    uint8_t  geo_mode;            // GEO_* the saved geometry came from
    uint8_t  ext_reserved;

    // --- Extension block 2.  Settings saved without it (stock firmware or
    // older API firmware) have no CRC; they load after the range checks and
    // get one on the next save.
    uint32_t ext2_magic;
    char     geo_model[41];       // IDENTIFY model / serial of the drive the
    char     geo_serial[21];      // geometry was set up for ("" = unknown or forced)
    uint8_t  ext2_reserved[2];
    uint32_t crc;                 // CRC-32 of every byte before this field (as stored)
} config_t;

// How the settings were loaded at boot (config_load_status).
#define CONFIG_LOAD_NONE      0   // nothing saved: defaults
#define CONFIG_LOAD_OK        1   // CRC verified, all fields in range
#define CONFIG_LOAD_LEGACY    2   // older layout without CRC, all fields in range
#define CONFIG_LOAD_CRC_ERROR 3   // CRC mismatch: defaults loaded
#define CONFIG_LOAD_INVALID   4   // a field out of range: defaults loaded

extern config_t config;

void config_load(void);
void config_save(void);
void config_defaults(void);
uint32_t config_clamp_timeout(uint32_t ms);

extern uint8_t     config_load_status;   // CONFIG_LOAD_*
extern const char *config_load_detail;   // offending field for CONFIG_LOAD_INVALID, else NULL
const char *config_load_name(uint8_t status);

// Hardware-independent helpers (config_check.c, unit-tested on the host).
uint32_t    config_crc32(const void *data, uint32_t len);
const char *config_validate(const config_t *c, bool ext2);   // NULL = valid, else field name
bool        config_chs_valid(uint32_t cyls, uint32_t heads, uint32_t spt);   // 1..65535 / 1..16 / 1..63
void        config_large_chs(uint16_t n_cyl, uint8_t n_hd, uint16_t *l_cyl, uint8_t *l_hd);

// Whose geometry is this?  Compares the drive now attached (IDENTIFY data)
// with the drive the configured geometry was set up for.
#define GEO_ID_UNKNOWN  0   // can't tell (no IDENTIFY data, or owner unknown and geometry manual)
#define GEO_ID_MATCH    1
#define GEO_ID_MISMATCH 2
int         config_geo_identity(const config_t *c, const char *model, const char *serial,
                                const uint16_t *id);
const char *config_geo_identity_name(int v);

#endif
