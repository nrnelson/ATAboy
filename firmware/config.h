#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_MAGIC     0x1DE45707
#define CONFIG_EXT_MAGIC 0xA7AB0E01   // extension block below is valid

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
} config_t;

extern config_t config;

void config_load(void);
void config_save(void);
void config_defaults(void);
uint32_t config_clamp_timeout(uint32_t ms);

#endif
