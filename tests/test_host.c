// Host-side unit tests for the hardware-independent firmware modules:
// the pass-through safety classifier, the JSON writer and the settings checks.
//   cc -I firmware -o /tmp/t tests/test_host.c firmware/ata_policy.c firmware/json.c
//      firmware/config_check.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "ata_policy.h"
#include "json.h"
#include "config.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_policy(void) {
    // reads that must work with write protect on
    const unsigned char reads[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x27, 0x2F, 0x40, 0x42, 0x10, 0x1F,
                                   0x70, 0x90, 0x91, 0xA1, 0xC4, 0xC8, 0xE5, 0xE7, 0xEA, 0xEC, 0xEF, 0xF5, 0xF8};
    for (size_t i = 0; i < sizeof(reads); i++) CHECK(ata_classify(reads[i], 0) == ATA_CLASS_READ);
    // writes
    const unsigned char writes[] = {0x30, 0x31, 0x34, 0x35, 0x39, 0x3C, 0x3F, 0xC5, 0xCA, 0xE8};
    for (size_t i = 0; i < sizeof(writes); i++) CHECK(ata_classify(writes[i], 0) == ATA_CLASS_WRITE);
    // dangerous: WRITE LONG, FORMAT TRACK, DOWNLOAD MICROCODE, SECURITY, SET MAX, TRIM, vendor, unknown
    const unsigned char danger[] = {0x32, 0x33, 0x50, 0x92, 0x93, 0xF1, 0xF2, 0xF3, 0xF4, 0xF6, 0xF9, 0x37,
                                    0x06, 0x45, 0xB4, 0x80, 0x8F, 0x9A, 0xFF, 0xA0, 0xE9};
    for (size_t i = 0; i < sizeof(danger); i++) CHECK(ata_classify(danger[i], 0) == ATA_CLASS_DANGEROUS);
    // SMART sub-commands
    CHECK(ata_classify(0xB0, 0xD0) == ATA_CLASS_READ);   // READ DATA
    CHECK(ata_classify(0xB0, 0xDA) == ATA_CLASS_READ);   // RETURN STATUS
    CHECK(ata_classify(0xB0, 0xD4) == ATA_CLASS_READ);   // self-test
    CHECK(ata_classify(0xB0, 0xD6) == ATA_CLASS_WRITE);  // WRITE LOG
    CHECK(ata_classify(0xB0, 0x00) == ATA_CLASS_DANGEROUS);
    // DCO
    CHECK(ata_classify(0xB1, 0xC2) == ATA_CLASS_READ);
    CHECK(ata_classify(0xB1, 0xC3) == ATA_CLASS_DANGEROUS);
    CHECK(ata_classify(0xB1, 0xC0) == ATA_CLASS_DANGEROUS);
    // every opcode classifies to a valid class
    for (int c = 0; c < 256; c++) CHECK(ata_classify((unsigned char)c, 0) <= ATA_CLASS_DANGEROUS);
}

static void test_json(void) {
    char buf[256];
    json_t j;
    j_init(&j, buf, sizeof(buf));
    j_obj(&j, NULL);
    j_str(&j, "s", "a\"b\\c\x01\xff");
    j_int(&j, "neg", -42);
    j_int(&j, "min", (int64_t)(-9223372036854775807LL - 1));
    j_uint(&j, "big", 18446744073709551615ULL);
    j_bool(&j, "t", true);
    j_null(&j, "n");
    j_arr(&j, "a");
    j_uint(&j, NULL, 1); j_uint(&j, NULL, 2);
    j_obj(&j, NULL); j_end(&j);
    j_end(&j);
    j_end(&j);
    buf[j.len] = '\0';
    const char *want = "{\"s\":\"a\\\"b\\\\c\\u0001\\u00ff\",\"neg\":-42,\"min\":-9223372036854775808,"
                       "\"big\":18446744073709551615,\"t\":true,\"n\":null,\"a\":[1,2,{}]}";
    CHECK(!j.overflow);
    CHECK(strcmp(buf, want) == 0);
    if (strcmp(buf, want)) printf("  got  %s\n  want %s\n", buf, want);

    // overflow never writes past cap, and mark/restore rolls back cleanly
    char small[40];
    memset(small, 'X', sizeof(small));
    j_init(&j, small, 30);
    j_obj(&j, NULL);
    j_arr(&j, "e");
    int kept = 0;
    for (int i = 0; i < 10; i++) {
        json_mark_t m = j_mark(&j);
        j_obj(&j, NULL); j_uint(&j, "i", (uint64_t)i); j_end(&j);
        if (j.overflow) { j_restore(&j, m); break; }
        kept++;
    }
    j.cap = 40;
    j_end(&j); j_end(&j);
    small[j.len < 40 ? j.len : 39] = '\0';
    CHECK(kept >= 1 && kept < 10);
    CHECK(small[j.len - 1] == '}' && small[j.len - 2] == ']');
    CHECK(strncmp(small, "{\"e\":[{\"i\":0}", 13) == 0);

    char sb[48];
    CHECK(strcmp(ata_status_bits(0x59, sb), "DRDY|DSC|DRQ|ERR") == 0);
    CHECK(strcmp(ata_error_bits(0x40, sb), "UNC") == 0);
    CHECK(strcmp(ata_status_bits(0x00, sb), "") == 0);
}

static void good_config(config_t *c) {
    memset(c, 0, sizeof(*c));
    c->magic = CONFIG_MAGIC;
    c->drive_write_protected = true;
    c->dev_base = 0xA0;
    c->ext_magic = CONFIG_EXT_MAGIC;
    c->read_timeout_ms = c->write_timeout_ms = 1000;
    c->ext2_magic = CONFIG_EXT2_MAGIC;
}

static void test_config(void) {
    CHECK(config_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(config_crc32("", 0) == 0);

    config_t c;
    good_config(&c);
    CHECK(config_validate(&c, true) == NULL);               // defaults, no geometry
    c.cyls = 989; c.heads = 10; c.spt = 34; c.geo_mode = GEO_NORMAL;
    CHECK(config_validate(&c, true) == NULL);
    c.heads = 17;  CHECK(config_validate(&c, true) && !strcmp(config_validate(&c, true), "chs"));
    c.heads = 16; c.spt = 64; CHECK(config_validate(&c, true) != NULL);
    c.spt = 63; c.cyls = 0; CHECK(config_validate(&c, true) != NULL);   // partial CHS
    c.cyls = 65535; CHECK(config_validate(&c, true) == NULL);
    good_config(&c);
    c.use_lba_mode = true; CHECK(config_validate(&c, true) && !strcmp(config_validate(&c, true), "lba_sectors"));
    c.lba_sectors = 1953525168ULL; CHECK(config_validate(&c, true) == NULL);
    c.lba_sectors = 1ULL << 48; CHECK(config_validate(&c, true) != NULL);
    good_config(&c);
    memset(&c.auto_mount, 2, 1); CHECK(config_validate(&c, true) && !strcmp(config_validate(&c, true), "auto_mount"));
    good_config(&c); c.dev_base = 0x00;   CHECK(config_validate(&c, true) != NULL);
    good_config(&c); c.read_timeout_ms = 49; CHECK(config_validate(&c, true) != NULL);
    good_config(&c); c.write_timeout_ms = 600001; CHECK(config_validate(&c, true) != NULL);
    good_config(&c); c.ecc_len = 65; CHECK(config_validate(&c, true) != NULL);
    good_config(&c); c.geo_mode = 5; CHECK(config_validate(&c, true) != NULL);
    good_config(&c); memset(c.geo_model, 'A', sizeof(c.geo_model));
    CHECK(config_validate(&c, true) != NULL);               // unterminated owner string
    CHECK(config_validate(&c, false) == NULL);              // ...ignored for older layouts

    CHECK(config_chs_valid(1, 1, 1) && config_chs_valid(65535, 16, 63));
    CHECK(!config_chs_valid(0, 1, 1) && !config_chs_valid(65536, 1, 1));
    CHECK(!config_chs_valid(1, 0, 1) && !config_chs_valid(1, 17, 1));
    CHECK(!config_chs_valid(1, 1, 0) && !config_chs_valid(1, 1, 64));

    // CRC as config_save stores it: over every byte before 'crc'
    good_config(&c);
    uint32_t crc = config_crc32(&c, (uint32_t)offsetof(config_t, crc));
    c.cyls = 1;
    CHECK(config_crc32(&c, (uint32_t)offsetof(config_t, crc)) != crc);
    CHECK(sizeof(config_t) <= 256);                         // one flash page

    // geometry owner
    uint16_t id[256];
    memset(id, 0, sizeof(id));
    id[1] = 989; id[3] = 10; id[6] = 34;
    good_config(&c);
    c.cyls = 989; c.heads = 10; c.spt = 34; c.geo_mode = GEO_NORMAL;
    strcpy(c.geo_model, "IBM-H2172-A2"); strcpy(c.geo_serial, "S1");
    CHECK(config_geo_identity(&c, "IBM-H2172-A2", "S1", id) == GEO_ID_MATCH);
    CHECK(config_geo_identity(&c, "IBM-H2172-A2", "S2", id) == GEO_ID_MISMATCH);
    CHECK(config_geo_identity(&c, "WDC WD10EZEX", "S1", id) == GEO_ID_MISMATCH);
    CHECK(config_geo_identity(&c, "x", "y", NULL) == GEO_ID_UNKNOWN);   // no IDENTIFY data
    // owner unknown (older settings): IDENTIFY-derived geometry must agree
    c.geo_model[0] = c.geo_serial[0] = '\0';
    CHECK(config_geo_identity(&c, "IBM-H2172-A2", "S1", id) == GEO_ID_MATCH);
    uint16_t big[256];
    memset(big, 0, sizeof(big));
    big[1] = 16383; big[3] = 16; big[6] = 63; big[49] = 0x0200; big[83] = 1 << 10;
    big[60] = 0xFFFF; big[61] = 0x0FFF;
    big[100] = 0x6DB0; big[101] = 0x7470;                    // 1953525168 sectors
    CHECK(config_geo_identity(&c, "WDC WD10EZEX", "S9", big) == GEO_ID_MISMATCH);   // the 336260-sector case
    c.geo_mode = GEO_LBA; c.use_lba_mode = true; c.lba_sectors = 1953525168ULL;
    CHECK(config_geo_identity(&c, "WDC WD10EZEX", "S9", big) == GEO_ID_MATCH);
    c.lba_sectors = 268435455ULL;
    CHECK(config_geo_identity(&c, "WDC WD10EZEX", "S9", big) == GEO_ID_MISMATCH);
    c.geo_mode = GEO_LARGE; c.use_lba_mode = false; c.cyls = 989; c.heads = 10; c.spt = 34;
    CHECK(config_geo_identity(&c, "IBM-H2172-A2", "S1", id) == GEO_ID_MATCH);
    c.geo_mode = GEO_MANUAL; c.cyls = 1000;
    CHECK(config_geo_identity(&c, "IBM-H2172-A2", "S1", id) == GEO_ID_UNKNOWN);
    CHECK(!strcmp(config_geo_identity_name(GEO_ID_MISMATCH), "mismatch"));

    uint16_t lc; uint8_t lh;
    config_large_chs(16383, 16, &lc, &lh);
    CHECK(lc == 16383 && lh == 16);
}

int main(void) {
    test_policy();
    test_json();
    test_config();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all host tests passed\n");
    return 0;
}
