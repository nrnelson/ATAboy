// Host-side unit tests for the hardware-independent firmware modules:
// the pass-through safety classifier, the JSON writer, the settings checks,
// CRC-32, the run-once request-id cache and the API serial-port framing.
//   cc -I firmware -o /tmp/t tests/test_host.c firmware/ata_policy.c firmware/json.c
//      firmware/config_check.c firmware/crc32.c firmware/rid_cache.c
//      firmware/apiframe.c firmware/api_proto.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "ata_policy.h"
#include "json.h"
#include "ide.h"
#include "config.h"
#include "crc32.h"
#include "rid_cache.h"
#include "apiframe.h"
#include "api_proto.h"

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

    char evb[96];
    CHECK(strcmp(ide_event_bits(0, evb), "") == 0);
    CHECK(strcmp(ide_event_bits(IDE_EV_HW_RESET, evb), "hw_reset") == 0);
    CHECK(strcmp(ide_event_bits(IDE_EV_HW_RESET | IDE_EV_RESET_FAILED, evb), "hw_reset|reset_failed") == 0);
    CHECK(strcmp(ide_event_bits(IDE_EV_SELF_RESET | IDE_EV_REINIT, evb), "self_reset|reinit") == 0);
    CHECK(strcmp(ide_event_bits(IDE_EV_REINIT | IDE_EV_REINIT_FAILED, evb), "reinit|reinit_failed") == 0);
    CHECK(strlen(ide_event_bits(0xFF, evb)) < sizeof(evb));
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

// Build a request frame as a host would.
static size_t make_req(uint8_t *f, uint16_t tag, uint16_t alloc, const uint8_t *body, uint16_t len) {
    uint8_t h[AF_HDR_LEN] = {AF_MAGIC, AF_REQ, AF_VERSION, 0, (uint8_t)(tag >> 8), (uint8_t)tag,
                             (uint8_t)(alloc >> 8), (uint8_t)alloc, (uint8_t)(len >> 8), (uint8_t)len};
    memcpy(f, h, AF_HDR_LEN);
    memcpy(f + AF_HDR_LEN, body, len);
    uint32_t c = crc32_update(0, f, AF_HDR_LEN + len);
    uint8_t *p = f + AF_HDR_LEN + len;
    p[0] = (uint8_t)(c >> 24); p[1] = (uint8_t)(c >> 16); p[2] = (uint8_t)(c >> 8); p[3] = (uint8_t)c;
    return AF_HDR_LEN + len + AF_CRC_LEN;
}

static void test_apiframe(void) {
    static uint8_t f[AF_HDR_LEN + AF_CDB_LEN + AF_MAX_BODY + AF_CRC_LEN + 64];
    const uint8_t cdb[16] = {0xC0, 0x01, 'A', 'X', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    af_rx_t rx;
    af_event_t ev;
    af_rx_reset(&rx);
    CHECK(af_rx_idle(&rx));

    // garbage (including a lone magic and a fake "A5 A5") before a good frame
    uint8_t g[] = {'A', 'T', '\r', AF_MAGIC, 'x', AF_MAGIC};
    memcpy(f, g, sizeof(g));
    size_t n = sizeof(g) + make_req(f + sizeof(g), 0x1234, 4096, cdb, 16);
    size_t used = af_rx_feed(&rx, f, n, &ev);
    CHECK(used == n && ev.kind == AF_EV_REQUEST && ev.tag == 0x1234 && ev.alloc == 4096);
    CHECK(memcmp(ev.cdb, cdb, 16) == 0 && af_rx_idle(&rx));

    // one byte at a time, then two frames back to back: the parser stops after the first
    n = make_req(f, 7, 0, cdb, 16);
    for (size_t i = 0; i < n; i++) {
        used = af_rx_feed(&rx, f + i, 1, &ev);
        CHECK(used == 1);
        CHECK(ev.kind == (i == n - 1 ? AF_EV_REQUEST : AF_EV_NONE));
        CHECK(af_rx_idle(&rx) == (i == n - 1));
    }
    size_t n2 = make_req(f + n, 8, 0, cdb, 16);
    used = af_rx_feed(&rx, f, n + n2, &ev);
    CHECK(used == n && ev.kind == AF_EV_REQUEST && ev.tag == 7);
    used = af_rx_feed(&rx, f + n, n2, &ev);
    CHECK(used == n2 && ev.kind == AF_EV_REQUEST && ev.tag == 8);

    // a flipped bit anywhere after the type byte: BAD (or, in the header, not taken as a frame)
    for (size_t bit = 16; bit < n * 8; bit += 7) {
        n = make_req(f, 9, 0, cdb, 16);
        f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        af_rx_reset(&rx);
        af_rx_feed(&rx, f, n, &ev);
        CHECK(ev.kind != AF_EV_REQUEST);
    }
    af_rx_reset(&rx);

    // bad version / too short a body: dropped, and the next good frame still parses
    n = make_req(f, 1, 0, cdb, 16);
    f[2] = 2;
    n2 = make_req(f + n, 2, 0, cdb, 16);
    used = af_rx_feed(&rx, f, n + n2, &ev);
    CHECK(ev.kind == AF_EV_REQUEST && ev.tag == 2 && used == n + n2);
    n = make_req(f, 3, 0, cdb, 15);
    af_rx_feed(&rx, f, n, &ev);
    CHECK(ev.kind == AF_EV_NONE);
    af_rx_reset(&rx);

    // data-out: CRC-checked, reported unsupported
    static uint8_t body[AF_CDB_LEN + AF_MAX_BODY + 1];
    memcpy(body, cdb, 16);
    for (size_t i = 16; i < sizeof(body); i++) body[i] = (uint8_t)i;
    n = make_req(f, 4, 0, body, AF_CDB_LEN + AF_MAX_BODY);
    used = af_rx_feed(&rx, f, n, &ev);
    CHECK(used == n && ev.kind == AF_EV_UNSUPPORTED && ev.tag == 4);
    n = make_req(f, 5, 0, body, sizeof(body));              // longer than allowed: not a frame
    af_rx_feed(&rx, f, AF_HDR_LEN, &ev);
    CHECK(ev.kind == AF_EV_NONE && af_rx_idle(&rx));

    // response: header, body and CRC
    memcpy(f + AF_HDR_LEN, "hello", 5);
    n = af_response(f, 0xBEEF, AF_ST_OK, 5);
    CHECK(n == AF_HDR_LEN + 5 + AF_CRC_LEN);
    CHECK(f[0] == AF_MAGIC && f[1] == AF_RSP && f[2] == AF_VERSION && f[3] == AF_ST_OK);
    CHECK(f[4] == 0xBE && f[5] == 0xEF && f[8] == 0 && f[9] == 5);
    uint32_t c = crc32_update(0, f, AF_HDR_LEN + 5);
    CHECK(f[n - 4] == (uint8_t)(c >> 24) && f[n - 1] == (uint8_t)c);
}

// Host half of the framing against the firmware half, both directions.
static void test_apiframe_host(void) {
    static uint8_t f[AF_HDR_LEN + AF_MAX_BODY + AF_CRC_LEN + 64];
    const uint8_t cdb[16] = {0xC0, 0x22, 'A', 'X', 0, 0, 1, 0, 0, 7, 0, 0, 0, 0, 0x3A, 0x98};
    af_rx_t rx;
    af_event_t ev;
    af_rx_reset(&rx);
    size_t n = af_request(f, 0xA55A, 4096, cdb);
    CHECK(n == AF_HDR_LEN + AF_CDB_LEN + AF_CRC_LEN);
    CHECK(af_rx_feed(&rx, f, n, &ev) == n && ev.kind == AF_EV_REQUEST && ev.tag == 0xA55A);
    CHECK(ev.alloc == 0 && memcmp(ev.cdb, cdb, 16) == 0);
    n = af_request(f, 1, 300, cdb);
    af_rx_feed(&rx, f, n, &ev);
    CHECK(ev.kind == AF_EV_REQUEST && ev.alloc == 300);

    // response scan: garbage, a lone magic, a fake "A5 Q", then the frame
    const uint8_t g[] = {'x', AF_MAGIC, AF_MAGIC, AF_REQ, 0};
    memcpy(f, g, sizeof(g));
    for (unsigned i = 0; i < 300; i++) f[sizeof(g) + AF_HDR_LEN + i] = (uint8_t)i;
    size_t fl = af_response(f + sizeof(g), 0x0102, AF_ST_OK, 300);
    n = sizeof(g) + fl;
    af_frame_t fr;
    bool found;
    // every truncation: never a frame, never drops a byte that could start one
    for (size_t cut = 0; cut < n; cut++) {
        size_t drop = af_scan_response(f, cut, &fr, &found);
        CHECK(!found && drop <= sizeof(g) && drop <= cut);
    }
    size_t drop = af_scan_response(f, n, &fr, &found);
    CHECK(found && drop == n && fr.crc_ok && fr.tag == 0x0102 && fr.status == AF_ST_OK && fr.len == 300);
    CHECK(fr.body == f + sizeof(g) + AF_HDR_LEN && fr.body[299] == (uint8_t)299);
    // flipped body bit: found, CRC bad
    f[sizeof(g) + AF_HDR_LEN + 10] ^= 4;
    af_scan_response(f, n, &fr, &found);
    CHECK(found && !fr.crc_ok);
    // oversized length field: skipped as not-a-frame
    fl = af_response(f, 9, AF_ST_OK, 0);
    f[8] = 0x20; f[9] = 0x01;                   // 8193 > AF_MAX_BODY
    drop = af_scan_response(f, fl, &fr, &found);
    CHECK(!found && drop == fl);
}

static void test_api_proto(void) {
    CHECK(strcmp(api_result_name(R_OK), "ok") == 0);
    CHECK(strcmp(api_result_name(R_GEO_MISMATCH), "geometry_mismatch") == 0);
    CHECK(strcmp(api_result_name(R_ID_CONFLICT), "request_id_conflict") == 0);
    CHECK(strcmp(api_result_name(99), "unknown") == 0 && strcmp(api_result_name(-1), "unknown") == 0);
    CHECK(strcmp(api_ide_result_name(APIIDE_TIMEOUT), "timeout") == 0);
    CHECK(api_op_by_name("hashes") == OP_HASHES && api_op_by_name("nope") == -1);
    CHECK(strcmp(api_op_name(OP_PARK), "park") == 0 && strcmp(api_op_name(0xEE), "unknown") == 0);
    for (int op = 0; op < 256; op++)                   // an op is never both
        CHECK(!(api_op_takes_rid((uint8_t)op) && api_op_read_only((uint8_t)op)));
    CHECK(api_op_takes_rid(OP_PARK) && api_op_takes_rid(OP_REBOOT) && !api_op_takes_rid(OP_READ));
    CHECK(api_op_read_only(OP_HASHES) && !api_op_read_only(OP_CORRUPT) && !api_op_takes_rid(OP_CORRUPT) == false);
}

int main(void) {
    test_policy();
    test_json();
    test_config();
    test_crc32();
    test_rid_cache();
    test_apiframe();
    test_apiframe_host();
    test_api_proto();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all host tests passed\n");
    return 0;
}
