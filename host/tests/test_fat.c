// whatfile and ata tests (no hardware).
//
//   test_fat CORPUS_DIR GOLDEN_DIR
//
// The golden files are the Python reference tool's whatfile output for the
// seed images (tests/gen_seeds.py --golden); the C library must match them
// byte for byte.  The ATA expectations below were taken from the Python
// tool's ATAboy.ata() with SG_IO stubbed out.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap + 1);
    size_t k;
    while (b && (k = fread(b + len, 1, cap - len, f)) > 0) {
        len += k;
        if (len == cap) { char *nb = realloc(b, 2 * cap + 1); if (!nb) { free(b); b = NULL; break; } b = nb; cap *= 2; }
    }
    fclose(f);
    if (b) b[len] = '\0';
    if (n) *n = len;
    return b;
}

static void spit(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

// ---------------------------------------------------------------------------
//  Golden comparison with the Python tool
// ---------------------------------------------------------------------------

static void golden(const char *corpus, const char *gold, const char *name) {
    char p[1024], g[1024];
    snprintf(p, sizeof(p), "%s/%s", corpus, name);
    snprintf(g, sizeof(g), "%s/%s.json", gold, name);
    size_t n;
    char *img = slurp(p, &n), *want = slurp(g, NULL);
    CHECK(img && want);
    if (!img || !want) { free(img); free(want); return; }
    size_t k = 0, sectors = n / 512 + 2;
    int64_t *lbas = malloc((sectors + 3) * sizeof(int64_t));
    for (size_t i = 0; i < sectors; i++) lbas[k++] = (int64_t)i;
    lbas[k++] = 100000;
    lbas[k++] = 1ll << 33;
    lbas[k++] = -1;
    cJSON *o = NULL;
    CHECK(ab_whatfile_mem((const uint8_t *)img, n, name, lbas, k, &o) == AB_OK);
    char *s = ab_json_print(o, 0);
    size_t wl = strlen(want);
    while (wl && (want[wl - 1] == '\n' || want[wl - 1] == '\r')) want[--wl] = '\0';
    bool same = s && !strcmp(s, want);
    if (!same) {
        printf("FAIL golden %s differs from the Python output; got:\n%s\n", name, s ? s : "(null)");
        failures++;
    }
    free(s);
    cJSON_Delete(o);
    // the file-based entry point gives the same result
    o = NULL;
    CHECK(ab_whatfile(p, lbas, k, &o) == AB_OK);
    if (o) cJSON_ReplaceItemInObjectCaseSensitive(o, "image", cJSON_CreateString(name));
    s = ab_json_print(o, 0);
    CHECK(s && !strcmp(s, want));
    free(s);
    cJSON_Delete(o);
    free(lbas);
    free(img);
    free(want);
}

// ---------------------------------------------------------------------------
//  Hand-built images
// ---------------------------------------------------------------------------

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

static void bpb(uint8_t *bs, unsigned spc, unsigned rsv, unsigned nfats, unsigned rootent, uint32_t total,
                uint32_t fatsz, bool fat32) {
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "TESTOEM ", 8);
    put16(bs + 11, 512);
    bs[13] = (uint8_t)spc;
    put16(bs + 14, rsv);
    bs[16] = (uint8_t)nfats;
    put16(bs + 17, rootent);
    if (total < 65536 && !fat32) put16(bs + 19, total); else put32(bs + 32, total);
    bs[21] = 0xF8;
    if (fat32) { put32(bs + 36, fatsz); put32(bs + 44, 2); } else put16(bs + 22, fatsz);
    bs[510] = 0x55; bs[511] = 0xAA;
}

static void dirent(uint8_t *e, const char *name11, uint8_t attr, uint32_t cluster, uint32_t size) {
    memcpy(e, name11, 11);
    e[11] = attr;
    put16(e + 26, cluster & 0xFFFF);
    put16(e + 20, cluster >> 16);
    put32(e + 28, size);
}

static cJSON *run(const uint8_t *img, size_t n, const int64_t *lbas, size_t k) {
    cJSON *o = NULL;
    int rc = ab_whatfile_mem(img, n, "t.img", lbas, k, &o);
    CHECK(rc == AB_OK);
    return o;
}

static const cJSON *result(const cJSON *o, int i) { return cJSON_GetArrayItem(ab_jobj_get(o, "results"), i); }
static const cJSON *volume(const cJSON *o) { return ab_jobj_get(cJSON_GetArrayItem(ab_jobj_get(o, "partitions"), 0), "volume"); }

// A BPB that promises a huge volume in a 4 KiB image must not make the
// library allocate or walk anything proportional to the promise.
static void test_huge_bpb(void) {
    const size_t n = 64 * 1024;
    uint8_t *img = calloc(1, n);
    bpb(img, 1, 32, 2, 0, 0xFFFFFFF0u, 0x00FFFFFF, true);      // ~4 G clusters, FATs of 8 GiB
    int64_t lbas[] = {0, 1, 32, 5000, 0x7FFFFFFF, 0xFFFFFFF0ll, 1ll << 40};
    cJSON *o = run(img, n, lbas, 7);
    const cJSON *v = volume(o);
    CHECK(!strcmp(ab_jstr(v, "fs", ""), "FAT32"));
    CHECK(!strcmp(ab_jstr(result(o, 0), "kind", ""), "boot_sector"));
    CHECK(!strcmp(ab_jstr(result(o, 2), "kind", ""), "fat"));
    CHECK(!strcmp(ab_jstr(result(o, 4), "kind", ""), "beyond_image"));
    cJSON_Delete(o);
    free(img);
}

// Directory nesting deeper than the walk's limit: reported, not followed.
static void test_deep_dirs(void) {
    const unsigned levels = 200, rsv = 1, fatsz = 2, data = rsv + fatsz + 1;   // 16 root entries = 1 sector
    size_t n = (size_t)(data + levels + 2) * 512;
    uint8_t *img = calloc(1, n);
    bpb(img, 1, rsv, 1, 16, 3000, fatsz, false);          // FAT12
    uint8_t *fat = img + rsv * 512;
    // FAT12 entries: every directory cluster is a one-cluster chain (EOC)
    for (unsigned c = 2; c < 2 + levels + 1; c++) {
        unsigned o = c + c / 2, v = 0xFFF;
        if (c & 1) { fat[o] = (uint8_t)((fat[o] & 0x0F) | (v << 4)); fat[o + 1] = (uint8_t)(v >> 4); }
        else { fat[o] = (uint8_t)v; fat[o + 1] = (uint8_t)((fat[o + 1] & 0xF0) | (v >> 8)); }
    }
    dirent(img + (rsv + fatsz) * 512, "D          ", 0x10, 2, 0);
    for (unsigned i = 0; i < levels; i++) {
        uint8_t *d = img + (size_t)(data + i) * 512;
        dirent(d, ".          ", 0x10, 2 + i, 0);
        dirent(d + 32, "..         ", 0x10, i ? 1 + i : 0, 0);
        dirent(d + 64, "D          ", 0x10, 3 + i, 0);
    }
    int64_t lbas[] = {(int64_t)data + 5, (int64_t)data + 150};
    cJSON *o = run(img, n, lbas, 2);
    const cJSON *v = volume(o);
    CHECK(ab_jbool(v, "index_truncated", false));
    CHECK(!strcmp(ab_jstr(v, "index_limit", ""), "directory depth"));
    const cJSON *r = result(o, 0);
    CHECK(!strcmp(ab_jstr(r, "kind", ""), "directory"));
    CHECK(!strcmp(ab_jstr(r, "path", ""), "/D/D/D/D/D/D"));
    CHECK(!strcmp(ab_jstr(result(o, 1), "kind", ""), "lost"));         // below the depth limit: not reached
    cJSON_Delete(o);
    free(img);
}

static void test_limits_and_layout(void) {
    // too many LBAs
    int64_t *many = calloc(AB_WHATFILE_MAX_LBAS + 1, sizeof(int64_t));
    cJSON *o = NULL;
    uint8_t blank[1024] = {0};
    CHECK(ab_whatfile_mem(blank, sizeof(blank), "b", many, AB_WHATFILE_MAX_LBAS + 1, &o) == AB_ERR_ARG && !o);
    free(many);
    // no partition table, no FAT; and an empty image
    int64_t l[] = {0, 1, 2, 0, -5};
    o = run(blank, sizeof(blank), l, 5);
    CHECK(!strcmp(ab_jstr(o, "scheme", ""), "none"));
    CHECK(cJSON_GetArraySize(ab_jobj_get(o, "results")) == 4);            // repeat dropped
    CHECK(!strcmp(ab_jstr(result(o, 0), "kind", ""), "unpartitioned"));
    CHECK(!strcmp(ab_jstr(result(o, 2), "kind", ""), "beyond_image"));
    CHECK(!strcmp(ab_jstr(result(o, 3), "kind", ""), "invalid"));
    cJSON_Delete(o);
    o = run(blank, 0, l, 1);
    CHECK(ab_jnum(o, "image_sectors", -1) == 0);
    cJSON_Delete(o);
    // missing image file
    o = NULL;
    CHECK(ab_whatfile("no-such-image.img", l, 1, &o) == AB_ERR_IO && !o);
    CHECK(strstr(ab_last_error(), "no-such-image.img") != NULL);
    // invalid BPBs are not FAT
    uint8_t bs[512] = {0};
    bpb(bs, 3, 1, 2, 224, 2880, 9, false);                              // 3 sectors/cluster
    o = run(bs, sizeof(bs), l, 1);
    CHECK(strcmp(ab_jstr(o, "scheme", ""), "superfloppy") != 0);
    cJSON_Delete(o);
    bpb(bs, 1, 1, 2, 224, 2880, 9, false);
    bs[21] = 0x12;                                                       // bad media byte
    o = run(bs, sizeof(bs), l, 1);
    CHECK(strcmp(ab_jstr(o, "scheme", ""), "superfloppy") != 0);
    cJSON_Delete(o);
    bs[21] = 0xF0;
    o = run(bs, sizeof(bs), l, 1);
    CHECK(!strcmp(ab_jstr(o, "scheme", ""), "superfloppy"));
    cJSON_Delete(o);
}

// Port of the Python test_whatfile_lbas.
static void test_lbas(void) {
    const char *jp = "test_fat_lbas.json", *mp = "test_fat_lbas.map";
    spit(jp, "[1, \"0x10\", {\"lba\": 5}]");
    int64_t seven = 7, *out = NULL;
    size_t n = 0;
    CHECK(ab_whatfile_lbas(&seven, 1, NULL, jp, &out, &n) == AB_OK);
    CHECK(n == 4 && out[0] == 7 && out[1] == 1 && out[2] == 16 && out[3] == 5);
    free(out);
    spit(jp, "{\"result\": \"errors\", \"bad_lbas\": [300, 301]}");
    CHECK(ab_whatfile_lbas(NULL, 0, NULL, jp, &out, &n) == AB_OK);
    CHECK(n == 2 && out[0] == 300 && out[1] == 301);
    free(out);
    spit(mp, "# Mapfile. Created by GNU ddrescue version 1.27\n"
             "# current_pos  current_status  current_pass\n"
             "0x00000000     +               1\n"
             "#      pos        size  status\n"
             "0x00000000  0x00000400  +\n"
             "0x00000400  0x00000200  -\n"
             "0x00000600  0x00000200  ?\n");
    CHECK(ab_whatfile_lbas(NULL, 0, mp, jp, &out, &n) == AB_OK);
    CHECK(n == 3 && out[0] == 2 && out[1] == 300 && out[2] == 301);
    free(out);
    spit(mp, "0x0 + 1\n0x00000000  0x00000800  *\n0x800 0x201 /\n");
    CHECK(ab_whatfile_lbas(NULL, 0, mp, NULL, &out, &n) == AB_OK);
    CHECK(n == 6 && out[0] == 0 && out[3] == 3 && out[4] == 4 && out[5] == 5);
    free(out);
    spit(jp, "{\"bad_lbas\": 5}");
    CHECK(ab_whatfile_lbas(NULL, 0, NULL, jp, &out, &n) == AB_ERR_ARG && !out);
    spit(mp, "# only comments\n");
    CHECK(ab_whatfile_lbas(NULL, 0, mp, NULL, &out, &n) == AB_ERR_ARG);
    CHECK(strstr(ab_last_error(), "not a ddrescue mapfile") != NULL);
    spit(mp, "0 + 1\n0 0xFFFFFFFFFFFF -\n");                          // absurd bad area: capped
    CHECK(ab_whatfile_lbas(NULL, 0, mp, NULL, &out, &n) == AB_ERR_ARG);
    remove(jp);
    remove(mp);
}

// ---------------------------------------------------------------------------
//  ATA pass-through without hardware
// ---------------------------------------------------------------------------

static void hexs(const uint8_t *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
}

static void cdb_is(const ab_ata_cmd *c, const char *want) {
    uint8_t cdb[16];
    char h[40];
    ab_ata_cdb(c, cdb);
    hexs(cdb, 16, h);
    if (strcmp(h, want)) { printf("FAIL cdb %s, want %s\n", h, want); failures++; }
}

static void test_ata_cdb(void) {
    ab_ata_cmd c;
    memset(&c, 0, sizeof(c));
    c.command = 0xEC; c.data_in = 512; c.ck_cond = true;                 // IDENTIFY DEVICE
    cdb_is(&c, "85082e0000000000000000000000ec00");
    memset(&c, 0, sizeof(c));
    c.command = 0x24; c.feature = 0xABCD; c.count = 0x0102; c.lba = 0x123456789All; c.device = 0x40;
    c.ext = true; c.data_in = 1024;                                      // READ SECTORS EXT, no CK_COND
    cdb_is(&c, "85090eabcd0102349a12780056402400");
    memset(&c, 0, sizeof(c));
    c.command = 0xB0; c.feature = 0xD8; c.lba = 0xC24F00; c.ck_cond = true;   // SMART ENABLE: non-data
    cdb_is(&c, "85062000d800000000004f00c200b000");
    static const uint8_t zero[512];
    memset(&c, 0, sizeof(c));
    c.command = 0x30; c.count = 1; c.lba = 5; c.device = 0xE0; c.data_out = zero; c.data_out_len = 512;
    c.ck_cond = true;                                                    // WRITE SECTORS: PIO out
    cdb_is(&c, "850a2600000001000500000000e03000");
    memset(&c, 0, sizeof(c));
    c.command = 0xEF; c.feature = -1; c.count = -2; c.lba = -1; c.ck_cond = true;   // Python's masking
    cdb_is(&c, "850620fffffffeffffffffffff00ef00");
}

static void sense_is(const char *sense_hex, const char *want) {
    uint8_t s[64];
    size_t n = strlen(sense_hex) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(sense_hex + 2 * i, "%2x", &v); s[i] = (uint8_t)v; }
    cJSON *j = ab_ata_sense_result(0xEC, s, n);
    char *t = ab_json_print(j, 0);
    if (!t || strcmp(t, want)) { printf("FAIL sense %s:\n  got  %s\n  want %s\n", sense_hex, t, want); failures++; }
    free(t);
    cJSON_Delete(j);
}

static void test_ata_sense(void) {
    sense_is("7201001d0000000e090c00000102030405060708e050",
             "{\"command\": 236, \"ok\": true, \"sense\": \"7201001d0000000e090c00000102030405060708e050\", \"sense_key\": 1, "
             "\"status\": 80, \"status_bits\": \"DRDY|DSC\", \"error\": 0, \"error_bits\": \"\", \"count\": 2, "
             "\"lba\": 525828, \"device\": 224}");
    sense_is("720b001d0000000e090c01040102030405060708e051",
             "{\"command\": 236, \"ok\": false, \"sense\": \"720b001d0000000e090c01040102030405060708e051\", \"sense_key\": 11, "
             "\"status\": 81, \"status_bits\": \"DRDY|DSC|ERR\", \"error\": 4, \"error_bits\": \"ABRT\", \"count\": 258, "
             "\"lba\": 7718107088388, \"device\": 224}");
    sense_is("7201001d00000014020400000000090c00000102030405060708e050",      // another descriptor first
             "{\"command\": 236, \"ok\": true, \"sense\": \"7201001d00000014020400000000090c00000102030405060708e050\", "
             "\"sense_key\": 1, \"status\": 80, \"status_bits\": \"DRDY|DSC\", \"error\": 0, \"error_bits\": \"\", "
             "\"count\": 2, \"lba\": 525828, \"device\": 224}");
    sense_is("7207270000000000", "{\"command\": 236, \"ok\": false, \"sense\": \"7207270000000000\", \"sense_key\": 7, \"blocked\": \"write_protected\"}");
    sense_is("700007000000000a000000002002", "{\"command\": 236, \"ok\": false, \"sense\": \"700007000000000a000000002002\", \"sense_key\": 7, \"blocked\": \"not_armed\"}");
    sense_is("7205240000000000", "{\"command\": 236, \"ok\": false, \"sense\": \"7205240000000000\", \"sense_key\": 5, \"blocked\": \"invalid_field\"}");
    sense_is("7203110000000000", "{\"command\": 236, \"ok\": false, \"sense\": \"7203110000000000\", \"sense_key\": 3, \"blocked\": \"rejected\"}");
    sense_is("7201001d0000000e090c00000102030405060708e0",                     // descriptor cut short
             "{\"command\": 236, \"ok\": false, \"sense\": \"7201001d0000000e090c00000102030405060708e0\", \"sense_key\": 1, \"blocked\": \"rejected\"}");
    sense_is("7201", "{\"command\": 236, \"ok\": false, \"sense\": \"7201\", \"sense_key\": 0, \"blocked\": \"rejected\"}");
    CHECK(ab_parse_ata_return((const uint8_t *)"\x70\x00\x01", 3) == NULL);
}

static void test_ata_nodev(void) {
    ab_ata_cmd c;
    memset(&c, 0, sizeof(c));
    c.command = 0xE5;
    cJSON *o = (cJSON *)1;
    uint8_t *data = (uint8_t *)1;
    size_t n = 1;
    int rc = ab_ata(NULL, &c, &o, &data, &n);
#if defined(__linux__)
    CHECK(rc == AB_ERR_TRANSPORT);                 // no SCSI node known: nothing is opened
#else
    CHECK(rc == AB_ERR_UNSUPPORTED);
#endif
    CHECK(o == NULL && data == NULL && n == 0);
    c.data_in = 1u << 24;
    CHECK(ab_ata(NULL, &c, &o, &data, &n) != AB_OK);
}

int main(int argc, char **argv) {
    if (argc >= 3) {
        static const char *const images[] = {"fat12_floppy.img", "fat16_mbr_logical.img", "fat16_superfloppy.img",
                                             "fat32_mbr.img", "wrong_types.img"};
        for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) golden(argv[1], argv[2], images[i]);
    } else {
        printf("(no corpus/golden directories given: golden comparison skipped)\n");
    }
    test_huge_bpb();
    test_deep_dirs();
    test_limits_and_layout();
    test_lbas();
    test_ata_cdb();
    test_ata_sense();
    test_ata_nodev();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all whatfile/ata tests passed\n");
    return 0;
}
