#include "config.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "ide.h"
#include <string.h>
#include <stddef.h>

#define FLASH_TARGET_OFFSET (2048 * 1024 - FLASH_SECTOR_SIZE)
static const uint8_t *flash_target = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

config_t config;
static mutex_t config_save_mtx;    // config_save() can be called from either core

uint8_t     config_load_status = CONFIG_LOAD_NONE;
const char *config_load_detail = NULL;

#define CONFIG_CRC_LEN ((uint32_t)offsetof(config_t, crc))

uint32_t config_clamp_timeout(uint32_t ms) {
    if (ms < TIMEOUT_MIN_MS) return TIMEOUT_MIN_MS;
    if (ms > TIMEOUT_MAX_MS) return TIMEOUT_MAX_MS;
    return ms;
}

static void config_ext_defaults(void) {
    config.ext_magic        = CONFIG_EXT_MAGIC;
    config.read_timeout_ms  = 1000;     // stock behaviour
    config.write_timeout_ms = 1000;
    config.ecc_len          = 0;
    config.no_retry         = false;
    config.geo_mode         = GEO_NONE;
    config.ext_reserved     = 0;
}

static void config_ext2_defaults(void) {
    config.ext2_magic = CONFIG_EXT2_MAGIC;
    memset(config.geo_model, 0, sizeof(config.geo_model));
    memset(config.geo_serial, 0, sizeof(config.geo_serial));
    memset(config.ext2_reserved, 0, sizeof(config.ext2_reserved));
    config.crc = 0;
}

void config_defaults(void) {
    config.main_selected = 0;
    config.feat_selected = 0;
    config.drive_write_protected = true;
    config.auto_mount = false;
    config.iordy_enabled = false;
    config.intrq_enabled = false;
    config.use_lba_mode = false;
    config.cyls = 0;
    config.heads = 0;
    config.spt = 0;
    config.lba_sectors = 0;
    config.dev_base = 0xA0;
    config_ext_defaults();
    config_ext2_defaults();
}

// Settings with a CRC (extension block 2) must match it.  Older layouts have
// none and are accepted when every field is in range; the next save adds
// the CRC.  A CRC mismatch or a field out of range loads the defaults, and
// config_load_status says so (STATUS "settings", menu notice).
void config_load(void) {
    mutex_init(&config_save_mtx);
    config_t tmp;
    memcpy(&tmp, flash_target, sizeof(config_t));
    config_load_detail = NULL;
    if (tmp.magic != CONFIG_MAGIC) {
        config_defaults();
        config_load_status = CONFIG_LOAD_NONE;
        return;
    }
    bool ext2 = tmp.ext_magic == CONFIG_EXT_MAGIC && tmp.ext2_magic == CONFIG_EXT2_MAGIC;
    if (ext2 && config_crc32(flash_target, CONFIG_CRC_LEN) != tmp.crc) {
        config_defaults();
        config_load_status = CONFIG_LOAD_CRC_ERROR;
        return;
    }
    config = tmp;
    if (config.ext_magic != CONFIG_EXT_MAGIC) {
        config_ext_defaults();
        // Settings from stock firmware: infer where the geometry came from.
        if (config.use_lba_mode && config.lba_sectors) config.geo_mode = GEO_LBA;
        else if (config.cyls && config.heads && config.spt) config.geo_mode = GEO_NORMAL;
    }
    if (!ext2) config_ext2_defaults();               // geometry owner unknown
    const char *bad = config_validate(&config, ext2);
    if (bad) {
        config_defaults();
        config_load_status = CONFIG_LOAD_INVALID;
        config_load_detail = bad;
        return;
    }
    // Menu cursor positions are cosmetic: reset rather than reject.
    if (config.main_selected > 15) config.main_selected = 0;
    if (config.feat_selected > 15) config.feat_selected = 0;
    config_load_status = ext2 ? CONFIG_LOAD_OK : CONFIG_LOAD_LEGACY;
}

// Both cores call multicore_lockout_victim_init(), so whichever core saves
// pauses the other while flash is erased/programmed.
void config_save(void) {
    mutex_enter_blocking(&config_save_mtx);
    ide_lock();      // the other core is paused below; don't freeze it mid-burst
    config.magic = CONFIG_MAGIC;
    config.ext_magic = CONFIG_EXT_MAGIC;
    config.ext2_magic = CONFIG_EXT2_MAGIC;
    static uint8_t buf[FLASH_PAGE_SIZE];
    _Static_assert(sizeof(config_t) <= FLASH_PAGE_SIZE, "config_t exceeds one flash page");
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &config, sizeof(config_t));
    // CRC over the bytes as stored (padding included), so load can check flash directly.
    uint32_t crc = config_crc32(buf, CONFIG_CRC_LEN);
    memcpy(buf + CONFIG_CRC_LEN, &crc, sizeof(crc));
    config.crc = crc;
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    ide_unlock();
    mutex_exit(&config_save_mtx);
}
