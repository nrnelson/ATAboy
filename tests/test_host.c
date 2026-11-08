// Host-side unit tests for the hardware-independent firmware modules:
// the pass-through safety classifier, the JSON writer, CRC-32, the run-once
// request-id cache.
//   cc -I firmware -o /tmp/t tests/test_host.c firmware/ata_policy.c firmware/json.c
//      firmware/crc32.c firmware/rid_cache.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include "ata_policy.h"
#include "json.h"
#include "crc32.h"
#include "rid_cache.h"

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

static void test_crc32(void) {
    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);     // the standard check value
    CHECK(crc32_update(0, "", 0) == 0);
    uint32_t c = crc32_update(0, "1234", 4);
    CHECK(crc32_update(c, "56789", 5) == 0xCBF43926u);          // incremental == one shot
}

static void test_rid_cache(void) {
    uint8_t a[16] = {0xC0, 0x12, 'A', 'X', 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
    uint8_t b[16];
    memcpy(b, a, 16);
    b[4] = 1;                                                   // same id, different request
    const uint8_t *r;
    uint32_t n;
    rid_clear();
    CHECK(rid_lookup(0x11223344, a, &r, &n) == RID_MISS);
    CHECK(rid_store(0x11223344, a, (const uint8_t *)"reply-a", 7));
    CHECK(rid_lookup(0x11223344, a, &r, &n) == RID_HIT && n == 7 && memcmp(r, "reply-a", 7) == 0);
    CHECK(rid_lookup(0x11223344, b, &r, &n) == RID_CONFLICT);
    CHECK(rid_lookup(0, a, &r, &n) == RID_MISS);                // id 0 = none, never cached
    CHECK(!rid_store(0, a, (const uint8_t *)"x", 1));
    static uint8_t big[RID_REPLY_MAX + 1];
    CHECK(!rid_store(5, a, big, sizeof(big)));                  // too long to keep
    CHECK(rid_store(6, a, big, RID_REPLY_MAX));
    // the oldest entry goes first
    for (uint32_t id = 100; id < 100 + RID_CACHE_ENTRIES; id++) CHECK(rid_store(id, a, (const uint8_t *)"z", 1));
    CHECK(rid_lookup(0x11223344, a, &r, &n) == RID_MISS);
    CHECK(rid_lookup(100, a, &r, &n) == RID_HIT);
    CHECK(rid_lookup(100 + RID_CACHE_ENTRIES - 1, a, &r, &n) == RID_HIT);
    rid_clear();
    CHECK(rid_lookup(100, a, &r, &n) == RID_MISS);
}

int main(void) {
    test_policy();
    test_json();
    test_crc32();
    test_rid_cache();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all host tests passed\n");
    return 0;
}
