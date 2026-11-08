// Hardware-independent checks on config_t: the CRC of the saved block, field
// range checks, and whether the configured geometry belongs to the drive now
// attached.  No SDK calls, so tests/test_host.c can exercise all of it.

#include "config.h"
#include <string.h>

// CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF): "123456789" -> CBF43926.
uint32_t config_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool config_chs_valid(uint32_t cyls, uint32_t heads, uint32_t spt) {
    return cyls >= 1 && cyls <= 65535 && heads >= 1 && heads <= 16 && spt >= 1 && spt <= 63;
}

// A bool loaded from flash may hold any byte; look at the byte, not the bool.
static bool bool_ok(const bool *b) {
    uint8_t v;
    memcpy(&v, b, 1);
    return v <= 1;
}

static bool str_ok(const char *s, size_t cap) {
    return memchr(s, '\0', cap) != NULL;
}

const char *config_validate(const config_t *c, bool ext2) {
    if (!bool_ok(&c->drive_write_protected)) return "write_protect";
    if (!bool_ok(&c->auto_mount))    return "auto_mount";
    if (!bool_ok(&c->iordy_enabled)) return "iordy";
    if (!bool_ok(&c->intrq_enabled)) return "intrq";
    if (!bool_ok(&c->use_lba_mode))  return "use_lba_mode";
    if (!bool_ok(&c->no_retry))      return "no_retry";
    if (c->dev_base != 0xA0 && c->dev_base != 0xB0) return "device";
    if (c->read_timeout_ms < TIMEOUT_MIN_MS || c->read_timeout_ms > TIMEOUT_MAX_MS) return "read_timeout_ms";
    if (c->write_timeout_ms < TIMEOUT_MIN_MS || c->write_timeout_ms > TIMEOUT_MAX_MS) return "write_timeout_ms";
    if (c->ecc_len > 64) return "ecc_len";
    if (c->geo_mode > GEO_MANUAL) return "geo_mode";

    uint8_t lba;
    memcpy(&lba, &c->use_lba_mode, 1);
    if (lba) {
        if (c->lba_sectors == 0 || c->lba_sectors >= (1ULL << 48)) return "lba_sectors";
    } else if (c->cyls || c->heads || c->spt) {      // all zero = no geometry
        if (!config_chs_valid(c->cyls, c->heads, c->spt)) return "chs";
    }
    if (ext2) {
        if (!str_ok(c->geo_model, sizeof(c->geo_model)))   return "geo_model";
        if (!str_ok(c->geo_serial, sizeof(c->geo_serial))) return "geo_serial";
    }
    return NULL;
}

void config_large_chs(uint16_t n_cyl, uint8_t n_hd, uint16_t *l_cyl, uint8_t *l_hd) {
    uint32_t total = (uint32_t)n_cyl * n_hd;
    uint8_t heads = n_hd ? n_hd : 1;
    while (heads < 255 && (total / heads) > 65535) heads++;
    *l_hd = heads;
    *l_cyl = (uint16_t)(total / heads);
}

int config_geo_identity(const config_t *c, const char *model, const char *serial, const uint16_t *id) {
    if (!id) return GEO_ID_UNKNOWN;                  // the attached drive gave no IDENTIFY data
    if (c->geo_model[0] || c->geo_serial[0])
        return (strcmp(c->geo_model, model) == 0 && strcmp(c->geo_serial, serial) == 0)
               ? GEO_ID_MATCH : GEO_ID_MISMATCH;

    // Owner unknown (settings from older firmware): geometry that came from
    // IDENTIFY must still agree with this drive's IDENTIFY.
    switch (c->geo_mode) {
    case GEO_NORMAL:
        return (c->cyls == id[1] && c->heads == id[3] && c->spt == id[6]) ? GEO_ID_MATCH : GEO_ID_MISMATCH;
    case GEO_LARGE: {
        uint16_t lc; uint8_t lh;
        config_large_chs(id[1], (uint8_t)id[3], &lc, &lh);
        return (c->cyls == lc && c->heads == lh && c->spt == id[6]) ? GEO_ID_MATCH : GEO_ID_MISMATCH;
    }
    case GEO_LBA: {
        uint64_t l48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[101] << 16) | (uint64_t)id[100];
        uint32_t l28 = id[60] | ((uint32_t)id[61] << 16);
        uint64_t cap = ((id[83] & (1 << 10)) && l48) ? l48 : l28;
        return c->lba_sectors == cap ? GEO_ID_MATCH : GEO_ID_MISMATCH;
    }
    default:
        return GEO_ID_UNKNOWN;                      // manual CHS set without IDENTIFY data
    }
}

const char *config_geo_identity_name(int v) {
    switch (v) {
    case GEO_ID_MATCH:    return "match";
    case GEO_ID_MISMATCH: return "mismatch";
    default:              return "unknown";
    }
}

const char *config_load_name(uint8_t status) {
    switch (status) {
    case CONFIG_LOAD_OK:        return "ok";
    case CONFIG_LOAD_LEGACY:    return "legacy";
    case CONFIG_LOAD_CRC_ERROR: return "crc_error";
    case CONFIG_LOAD_INVALID:   return "invalid";
    default:                    return "none";
    }
}
