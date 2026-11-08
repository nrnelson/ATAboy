#include "config.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include <string.h>

#define FLASH_TARGET_OFFSET (2048 * 1024 - FLASH_SECTOR_SIZE)
static const uint8_t *flash_target = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

config_t config;
static mutex_t config_save_mtx;    // config_save() can be called from either core

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
}

void config_load(void) {
    mutex_init(&config_save_mtx);
    config_t tmp;
    memcpy(&tmp, flash_target, sizeof(config_t));
    if (tmp.magic == CONFIG_MAGIC) {
        config = tmp;
        if (config.ext_magic != CONFIG_EXT_MAGIC) {
            config_ext_defaults();
            // Settings from stock firmware: infer where the geometry came from.
            if (config.use_lba_mode && config.lba_sectors) config.geo_mode = GEO_LBA;
            else if (config.cyls && config.heads && config.spt) config.geo_mode = GEO_NORMAL;
        }
        config.read_timeout_ms  = config_clamp_timeout(config.read_timeout_ms);
        config.write_timeout_ms = config_clamp_timeout(config.write_timeout_ms);
        if (config.dev_base != 0xA0 && config.dev_base != 0xB0) config.dev_base = 0xA0;
    } else {
        config_defaults();
    }
}

// Both cores call multicore_lockout_victim_init(), so whichever core saves
// pauses the other while flash is erased/programmed.
void config_save(void) {
    mutex_enter_blocking(&config_save_mtx);
    config.magic = CONFIG_MAGIC;
    config.ext_magic = CONFIG_EXT_MAGIC;
    static uint8_t buf[FLASH_PAGE_SIZE];
    _Static_assert(sizeof(config_t) <= FLASH_PAGE_SIZE, "config_t exceeds one flash page");
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &config, sizeof(config_t));
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    mutex_exit(&config_save_mtx);
}
