// image, rescue and hashes against a fake device with bad sectors
// (ports of tests/test_host_lib.py TestRescue, TestHashes, TestMisc image tests).
#include "fake_dev.h"
#include "api_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------

static void blank_image(const char *path, unsigned sectors) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    static const uint8_t z[512];
    for (unsigned i = 0; i < sectors; i++) fwrite(z, 1, 512, f);
    fclose(f);
}

static void put_sector(const char *path, uint64_t lba, const uint8_t *data) {
    FILE *f = fopen(path, "r+b");
    if (!f) { perror(path); exit(1); }
    ab_fseek(f, (long long)(lba * 512), SEEK_SET);
    fwrite(data, 1, 512, f);
    fclose(f);
}

static bool image_sector(const char *path, uint64_t lba, uint8_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = ab_fseek(f, (long long)(lba * 512), SEEK_SET) == 0 && fread(out, 1, 512, f) == 512;
    fclose(f);
    return ok;
}

static bool sector_is(const char *path, uint64_t lba, const uint8_t *want) {
    uint8_t got[512];
    return image_sector(path, lba, got) && !memcmp(got, want, 512);
}
static bool sector_is_data(const char *path, uint64_t lba) {
    uint8_t s[512];
    fake_sector(lba, s);
    return sector_is(path, lba, s);
}
static bool sector_is_zero(const char *path, uint64_t lba) {
    static const uint8_t z[512];
    return sector_is(path, lba, z);
}

typedef struct { uint64_t pos, size; char st; } blk;

static void write_map(const char *path, const blk *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs("# Mapfile. Created by GNU ddrescue version 1.27\n# current_pos  current_status  current_pass\n"
          "0x00000000     +               1\n#      pos        size  status\n", f);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "0x%08llX  0x%08llX  %c\n", (unsigned long long)b[i].pos, (unsigned long long)b[i].size, b[i].st);
    fclose(f);
}

static char map_status(const char *path, uint64_t lba) {
    ab_mapfile *m = NULL;
    if (ab_mapfile_load(path, &m) != AB_OK) return 'E';
    char c = ab_mapfile_status_at(m, lba);
    ab_mapfile_free(m);
    return c;
}

static bool map_blocks(const char *path, const blk *want, size_t n) {
    ab_mapfile *m = NULL;
    if (ab_mapfile_load(path, &m) != AB_OK) return false;
    size_t got;
    const ab_mapblock *b = ab_mapfile_blocks(m, &got);
    bool ok = got == n;
    for (size_t i = 0; ok && i < n; i++) ok = b[i].pos == want[i].pos && b[i].size == want[i].size && b[i].status == want[i].st;
    ab_mapfile_free(m);
    return ok;
}

static bool bad_sectors_are(const char *path, const uint64_t *want, size_t n) {
    ab_mapfile *m = NULL;
    if (ab_mapfile_load(path, &m) != AB_OK) return false;
    uint64_t *v;
    size_t got;
    bool ok = ab_mapfile_bad_sectors(m, 0, false, &v, &got) == AB_OK && got == n;
    for (size_t i = 0; ok && i < n; i++) ok = v[i] == want[i];
    free(v);
    ab_mapfile_free(m);
    return ok;
}

// JSON array of numbers equals want[]
static bool arr_is(const cJSON *a, const double *want, int n) {
    if (cJSON_GetArraySize(a) != n) return false;
    int i = 0;
    for (const cJSON *x = a ? a->child : NULL; x; x = x->next, i++)
        if (!cJSON_IsNumber(x) || x->valuedouble != want[i]) return false;
    return true;
}
#define ARR(a, ...) arr_is(a, (double[]){__VA_ARGS__}, (int)(sizeof((double[]){__VA_ARGS__}) / sizeof(double)))

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static size_t count_op(const fake_dev *f, uint8_t op) {
    size_t n = 0;
    for (size_t i = 0; i < f->ncalls; i++) n += f->calls[i].op == op;
    return n;
}

static bool read_call(const fake_dev *f, uint32_t lba, unsigned count, uint8_t flags, uint32_t tmo) {
    for (size_t i = 0; i < f->ncalls; i++) {
        const uint8_t *a = f->calls[i].a;
        if (f->calls[i].op == OP_READ && ab_be32(a) == lba && ab_be16(a + 4) == count && a[6] == flags &&
            ab_be32(a + 8) == tmo)
            return true;
    }
    return false;
}

static void progress_ignore(void *ctx, const char *line) { (void)ctx; (void)line; }

// ---------------------------------------------------------------------------
//  hashes
// ---------------------------------------------------------------------------

static void read_n(ab_dev *d, uint32_t lba, unsigned n, bool through_error) {
    ab_ataresult a;
    ab_reply r;
    cJSON *j = NULL;
    memset(&r, 0, offsetof(ab_reply, raw));
    ab_read(d, lba, n, false, through_error, 0, &a, &r, &j);
    ab_reply_free(&r);
    cJSON_Delete(j);
}

static void test_hashes_parse(void) {
    uint8_t p[32 + 2 * 48];
    memset(p, 0, sizeof(p));
    ab_put_be32(p, 7);           // newest
    ab_put_be32(p + 4, 6);       // oldest
    ab_put_be32(p + 8, 8);       // next
    ab_put_be16(p + 12, 2);      // n
    ab_put_be16(p + 14, 48);     // rec_len: longer than 44 (newer firmware) is fine
    ab_put_be16(p + 16, 1024);
    p[18] = 1;
    ab_put_be32(p + 24, 35);     // busy_us = 35
    for (int i = 0; i < 2; i++) {
        uint8_t *r = p + 32 + 48 * i;
        ab_put_be32(r, 6 + (uint32_t)i);
        ab_put_be32(r + 4, 100 + (uint32_t)i);
        r[8] = i ? 9 : 1;
        r[9] = (uint8_t)i;
        for (int k = 0; k < 32; k++) r[12 + k] = (uint8_t)(k + i);
    }
    cJSON *head, *recs;
    uint32_t next;
    CHECK(ab_hashes_parse(p, sizeof(p), &head, &recs, &next) == AB_OK);
    CHECK(next == 8);
    char *s = ab_json_print(head, 0);
    CHECK(s && !strcmp(s, "{\"newest\": 7, \"oldest\": 6, \"capacity\": 1024, \"active\": true, \"busy_us\": 35, "
                          "\"us_per_sector\": 5}"));
    free(s);
    s = ab_json_print(recs, 0);
    CHECK(s && !strcmp(s, "[{\"seq\": 6, \"lba\": 100, \"source\": \"usb\", \"err_data\": false, \"sha256\": "
                          "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"}, "
                          "{\"seq\": 7, \"lba\": 101, \"source\": \"source_9\", \"err_data\": true, \"sha256\": "
                          "\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\"}]"));
    free(s);
    cJSON_Delete(head);
    cJSON_Delete(recs);
    // malformed: short header, short record length, records beyond the payload
    CHECK(ab_hashes_parse(p, 31, &head, &recs, &next) == AB_ERR_TRANSPORT && !head && !recs);
    ab_put_be16(p + 14, 43);
    CHECK(ab_hashes_parse(p, sizeof(p), &head, &recs, &next) == AB_ERR_TRANSPORT);
    ab_put_be16(p + 14, 48);
    CHECK(ab_hashes_parse(p, sizeof(p) - 1, &head, &recs, &next) == AB_ERR_TRANSPORT);
    ab_put_be16(p + 12, 0xFFFF);
    CHECK(ab_hashes_parse(p, sizeof(p), &head, &recs, &next) == AB_ERR_TRANSPORT);
    // no sectors hashed: us_per_sector null
    memset(p, 0, 32);
    ab_put_be16(p + 14, 44);
    CHECK(ab_hashes_parse(p, 32, &head, &recs, &next) == AB_OK);
    CHECK(cJSON_IsNull(ab_jobj_get(head, "us_per_sector")) && !ab_jbool(head, "active", true));
    cJSON_Delete(head);
    cJSON_Delete(recs);
}

static void test_hashes_records(void) {
    fake_dev f;
    fake_init(&f, 4096);
    ab_dev *d = fake_open(&f);
    read_n(d, 10, 4, false);
    cJSON *j = NULL;
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    CHECK(ab_jbool(j, "active", false));
    CHECK(ab_jnum(j, "newest", 0) == 4);
    CHECK(ab_jnum(j, "us_per_sector", 0) == 10.0);
    const cJSON *recs = ab_jobj_get(j, "records");
    CHECK(cJSON_GetArraySize(recs) == 4);
    int i = 0;
    for (const cJSON *x = recs ? recs->child : NULL; x; x = x->next, i++) {
        CHECK(ab_jnum(x, "lba", 0) == 10 + i && ab_jnum(x, "seq", 0) == 1 + i);
        uint8_t s[512], dg[32];
        char hx[65];
        fake_sector(10 + (uint64_t)i, s);
        ab_sha256(s, 512, dg);
        for (int k = 0; k < 32; k++) snprintf(hx + 2 * k, 3, "%02x", dg[k]);
        CHECK(!strcmp(ab_jstr(x, "sha256", ""), hx));
        CHECK(!strcmp(ab_jstr(x, "source", ""), "api_read"));
        CHECK(!ab_jbool(x, "err_data", true));
    }
    cJSON_Delete(j);
    ab_close(d);
    fake_free(&f);
}

static void test_hashes_paging(void) {
    fake_dev f;
    fake_init(&f, 4096);
    ab_dev *d = fake_open(&f);
    for (uint32_t lba = 0; lba < 300; lba += 7) read_n(d, lba, 7, false);   // 301 records
    size_t before = count_op(&f, OP_HASHES);
    cJSON *j = NULL;
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    const cJSON *recs = ab_jobj_get(j, "records");
    CHECK(cJSON_GetArraySize(recs) == 301);
    int i = 1;
    bool seq_ok = true;
    for (const cJSON *x = recs ? recs->child : NULL; x; x = x->next, i++) seq_ok &= ab_jnum(x, "seq", 0) == i;
    CHECK(seq_ok);
    CHECK(count_op(&f, OP_HASHES) - before == 4);      // 92 records per page
    cJSON_Delete(j);
    CHECK(ab_hashes(d, 290, false, false, 0, 1, &j) == AB_OK);
    CHECK(cJSON_GetArraySize(ab_jobj_get(j, "records")) == 12);
    cJSON_Delete(j);
    CHECK(ab_hashes(d, 0, false, true, 20, 3, &j) == AB_OK);
    recs = ab_jobj_get(j, "records");
    CHECK(cJSON_GetArraySize(recs) == 3);
    i = 20;
    for (const cJSON *x = recs ? recs->child : NULL; x; x = x->next, i++) CHECK(ab_jnum(x, "lba", 0) == i);
    cJSON_Delete(j);
    ab_close(d);
    fake_free(&f);
}

static void test_hashes_ring_wraps(void) {
    fake_dev f;
    fake_init(&f, 4096);
    f.hash_capacity = 50;
    ab_dev *d = fake_open(&f);
    for (uint32_t lba = 0; lba < 70; lba += 7) read_n(d, lba, 7, false);
    cJSON *j = NULL;
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    CHECK(ab_jnum(j, "newest", 0) == 70);
    CHECK(ab_jnum(j, "oldest", 0) == 21);
    const cJSON *r = ab_jobj_get(j, "records");
    CHECK(r && r->child && ab_jnum(r->child, "lba", 0) == 20);
    cJSON_Delete(j);
    ab_close(d);
    fake_free(&f);
}

static void test_hashes_err_data_and_clear(void) {
    fake_dev f;
    fake_init(&f, 4096);
    fake_bad(&f, 5, FK_UNC);
    ab_dev *d = fake_open(&f);
    read_n(d, 3, 4, true);
    cJSON *j = NULL;
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    const cJSON *r = ab_jobj_get(j, "records");
    CHECK(cJSON_GetArraySize(r) == 3);
    double lb[3] = {3, 4, 5};
    bool ed[3] = {false, false, true};
    int i = 0;
    for (const cJSON *x = r ? r->child : NULL; x && i < 3; x = x->next, i++)
        CHECK(ab_jnum(x, "lba", 0) == lb[i] && ab_jbool(x, "err_data", !ed[i]) == ed[i]);
    cJSON_Delete(j);
    CHECK(ab_hashes(d, 0, true, false, 0, 1, &j) == AB_OK);
    CHECK(ab_jbool(j, "cleared", false));
    CHECK(cJSON_GetArraySize(ab_jobj_get(j, "records")) == 0);
    char *s = ab_json_print(j, 0);
    CHECK(s && strstr(s, "\"records\": [], \"cleared\": true}"));
    free(s);
    cJSON_Delete(j);
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    CHECK(ab_jnum(j, "newest", -1) == 0);
    cJSON_Delete(j);
    ab_close(d);
    fake_free(&f);
}

static void test_hashes_compare(void) {
    fake_dev f;
    fake_init(&f, 4096);
    ab_dev *d = fake_open(&f);
    read_n(d, 0, 6, false);
    const char *img = "t_rec_d.img";
    blank_image(img, 5);
    for (uint64_t lba = 0; lba < 5; lba++) {
        uint8_t s[512] = {0};
        if (lba != 2) fake_sector(lba, s);
        put_sector(img, lba, s);
    }
    cJSON *j = NULL, *c = NULL;
    CHECK(ab_hashes(d, 0, false, false, 0, 1, &j) == AB_OK);
    CHECK(ab_compare_hashes(ab_jobj_get(j, "records"), img, &c) == AB_OK);
    CHECK(ab_jnum(c, "match", 0) == 4);
    const cJSON *df = ab_jobj_get(c, "differ");
    CHECK(cJSON_GetArraySize(df) == 1 && ab_jnum(df->child, "lba", 0) == 2);
    CHECK(ab_jnum(c, "beyond_end", 0) == 1);
    CHECK(!strcmp(ab_jstr(c, "image", ""), img));
    cJSON_Delete(c);
    CHECK(ab_compare_hashes(ab_jobj_get(j, "records"), "t_rec_missing.img", &c) == AB_ERR_IO && !c);
    cJSON_Delete(j);
    remove(img);
    ab_close(d);
    fake_free(&f);
}

// ---------------------------------------------------------------------------
//  image
// ---------------------------------------------------------------------------

static void test_image_through_api(void) {
    fake_dev f;
    fake_init(&f, 4096);
    fake_bad(&f, 20, FK_UNC);
    ab_dev *d = fake_open(&f);
    const char *out = "t_rec_x.img";
    remove(out);
    remove("t_rec_x.img.map");
    ab_image_opts o = {0};
    o.has_count = true;
    o.count = 30;
    o.progress = progress_ignore;
    cJSON *res = NULL;
    CHECK(ab_image(d, out, &o, &res) == AB_OK);
    CHECK(!strcmp(ab_jstr(res, "via", ""), "api"));
    CHECK(!strcmp(ab_jstr(res, "result", ""), "errors"));
    CHECK(ARR(ab_jobj_get(res, "bad_lbas"), 20));
    CHECK(ab_jnum(res, "imaged_sectors", 0) == 29);
    CHECK(ab_jbool(res, "complete", false));
    CHECK(!strcmp(ab_jstr(res, "mapfile", ""), "t_rec_x.img.map"));
    char *s = ab_json_print(ab_jobj_get(res, "map"), 0);
    CHECK(s && !strcmp(s, "{\"+\": 14848, \"-\": 512, \"?\": 2081792}"));
    free(s);
    for (uint64_t lba = 0; lba < 30; lba++) CHECK(lba == 20 ? sector_is_zero(out, lba) : sector_is_data(out, lba));
    bool all_small = true;
    for (size_t i = 0; i < f.ncalls; i++)
        if (f.calls[i].op == OP_READ && ab_be16(f.calls[i].a + 4) > 7) all_small = false;
    CHECK(all_small);
    CHECK(map_status("t_rec_x.img.map", 20) == '-');
    FILE *fi = fopen(out, "rb");
    CHECK(fi && ab_fseek(fi, 0, SEEK_END) == 0 && ab_ftell(fi) == 4096 * 512);    // full-size (sparse) image
    if (fi) fclose(fi);
    cJSON_Delete(res);

    // resume: only the untried rest of the requested range is read
    size_t reads_before = count_op(&f, OP_READ);
    o.start = 25;
    o.count = 10;
    CHECK(ab_image(d, out, &o, &res) == AB_OK);
    CHECK(ab_jnum(res, "imaged_sectors", 0) == 5);              // 30..34
    CHECK(count_op(&f, OP_READ) - reads_before == 1);
    CHECK(sector_is_data(out, 34) && sector_is_zero(out, 35));
    CHECK(map_blocks("t_rec_x.img.map", (blk[]){{0, 20 * 512, '+'}, {20 * 512, 512, '-'}, {21 * 512, 14 * 512, '+'},
                                                 {35 * 512, (4096 - 35) * 512, '?'}}, 4));
    cJSON_Delete(res);
    remove(out);
    remove("t_rec_x.img.map");
    ab_close(d);
    fake_free(&f);
}

static void test_image_without_geometry(void) {
    fake_dev f;
    fake_init(&f, 4096);
    f.geometry_valid = false;
    ab_dev *d = fake_open(&f);
    cJSON *res = NULL;
    CHECK(ab_image(d, "t_rec_nogeo.img", NULL, &res) == AB_ERR_API);
    CHECK(!strcmp(ab_jstr(res, "result", ""), "no_geometry"));
    CHECK(strstr(ab_errmsg(d), "no_geometry") != NULL);
    CHECK(!file_exists("t_rec_nogeo.img") && !file_exists("t_rec_nogeo.img.map"));
    cJSON_Delete(res);
    ab_close(d);
    fake_free(&f);
}

static int hook_interrupt_at_40(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f; (void)sense;
    if (op == OP_READ && ab_be32(a) == 40) ab_interrupt();
    return 0;
}

static void test_image_interrupt(void) {
    fake_dev f;
    fake_init(&f, 4096);
    ab_dev *d = fake_open(&f);
    const char *out = "t_rec_int.img";
    remove(out);
    remove("t_rec_int.img.map");
    f.hook = hook_interrupt_at_40;
    ab_image_opts o = {0};
    o.chunk_sectors = 8;
    cJSON *res = NULL;
    CHECK(ab_image(d, out, &o, &res) == AB_OK);
    ab_interrupt_reset();
    CHECK(!strcmp(ab_jstr(res, "result", ""), "interrupted"));
    CHECK(!ab_jbool(res, "complete", true));
    CHECK(ab_jnum(res, "imaged_sectors", 0) == 48);            // the chunk in flight completes
    CHECK(map_blocks("t_rec_int.img.map", (blk[]){{0, 48 * 512, '+'}, {48 * 512, (4096 - 48) * 512, '?'}}, 2));
    cJSON_Delete(res);
    remove(out);
    remove("t_rec_int.img.map");
    ab_close(d);
    fake_free(&f);
}

static int hook_transport_fail_at_64(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f; (void)sense;
    if (op == OP_READ && ab_be32(a) >= 64) { ab_fail(AB_ERR_TRANSPORT, "device went away"); return AB_ERR_TRANSPORT; }
    return 0;
}

static void test_image_transport_error_saves_map(void) {
    fake_dev f;
    fake_init(&f, 4096);
    ab_dev *d = fake_open(&f);
    const char *out = "t_rec_te.img";
    remove(out);
    remove("t_rec_te.img.map");
    f.hook = hook_transport_fail_at_64;
    ab_image_opts o = {0};
    o.chunk_sectors = 16;
    cJSON *res = NULL;
    CHECK(ab_image(d, out, &o, &res) == AB_ERR_TRANSPORT && !res);
    CHECK(strstr(ab_errmsg(d), "went away") != NULL);
    CHECK(map_blocks("t_rec_te.img.map", (blk[]){{0, 64 * 512, '+'}, {64 * 512, (4096 - 64) * 512, '?'}}, 2));
    CHECK(sector_is_data(out, 63));
    remove(out);
    remove("t_rec_te.img.map");
    ab_close(d);
    fake_free(&f);
}

#if defined(__linux__)
// The USB-disk path, with a regular file standing in for /dev/sdX.
static void test_image_usb_disk(void) {
    const char *disk = "t_rec_fake_disk.bin";
    FILE *fd = fopen(disk, "wb");
    if (!fd) return;
    for (uint64_t lba = 0; lba < 300; lba++) { uint8_t s[512]; fake_sector(lba, s); fwrite(s, 1, 512, fd); }
    fclose(fd);
    ab_blk *probe = NULL;
    if (ab_blk_open(disk, &probe) != AB_OK) {
        printf("  (skipping the USB-disk image test: O_DIRECT not supported here)\n");
        remove(disk);
        return;
    }
    ab_blk_close(probe);
    fake_dev f;
    fake_init(&f, 300);
    f.mounted = true;
    ab_dev *d = fake_open(&f);
    d->have_info = true;
    snprintf(d->info.block, sizeof(d->info.block), "%s", disk);
    const char *out = "t_rec_usb.img";
    remove(out);
    remove("t_rec_usb.img.map");
    cJSON *res = NULL;
    CHECK(ab_image(d, out, NULL, &res) == AB_OK);
    CHECK(!strcmp(ab_jstr(res, "via", ""), "usb_disk"));
    CHECK(ab_jnum(res, "imaged_sectors", 0) == 300);
    CHECK(count_op(&f, OP_READ) == 0);
    for (uint64_t lba = 0; lba < 300; lba += 37) CHECK(sector_is_data(out, lba));
    CHECK(sector_is_data(out, 299));
    CHECK(map_blocks("t_rec_usb.img.map", (blk[]){{0, 300 * 512, '+'}}, 1));
    cJSON_Delete(res);
    // not mounted: through the API even though the block device is readable
    remove(out);
    remove("t_rec_usb.img.map");
    f.mounted = false;
    CHECK(ab_image(d, out, NULL, &res) == AB_OK);
    CHECK(!strcmp(ab_jstr(res, "via", ""), "api"));
    cJSON_Delete(res);
    remove(out);
    remove("t_rec_usb.img.map");
    remove(disk);
    ab_close(d);
    fake_free(&f);
}
#endif

// ---------------------------------------------------------------------------
//  rescue
// ---------------------------------------------------------------------------

#define SECTORS 64
static const char *IMG = "t_rec_disk.img", *MAP = "t_rec_disk.map", *SIDE = "t_rec_disk.img.readlong.json";

static ab_dev *rescue_setup(fake_dev *f) {
    fake_init(f, 4096);
    remove(SIDE);
    blank_image(IMG, SECTORS);
    for (uint64_t s = 0; s < 10; s++) { uint8_t b[512]; fake_sector(s, b); put_sector(IMG, s, b); }
    // sectors 0..9 good in the image; 10..13 were tried and failed
    write_map(MAP, (blk[]){{0, 10 * 512, '+'}, {10 * 512, 4 * 512, '-'}, {14 * 512, (SECTORS - 14) * 512, '+'}}, 3);
    return fake_open(f);
}

static void rescue_teardown(ab_dev *d, fake_dev *f) {
    ab_close(d);
    fake_free(f);
    remove(IMG);
    remove(MAP);
    remove(SIDE);
}

static ab_rescue_opts ropts(void) {
    ab_rescue_opts o;
    ab_rescue_opts_init(&o);
    o.progress = progress_ignore;
    return o;
}

static cJSON *load_json(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

static void test_rescue_all_passes(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    // 10 recovers in pass 1, 11 only with retries, 12 only via READ LONG, 13 never
    fake_bad(&f, 11, FK_NORETRY);
    fake_bad(&f, 12, FK_UNC);
    fake_bad(&f, 13, FK_DEAD);
    fake_flaky(&f, 12);
    ab_rescue_opts o = ropts();
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(ab_jnum(res, "candidates", 0) == 4);
    CHECK(ARR(ab_jobj_get(res, "recovered_no_retry"), 10));
    CHECK(ARR(ab_jobj_get(res, "recovered_retry"), 11));
    const cJSON *rl = ab_jobj_get(res, "readlong");
    CHECK(cJSON_GetArraySize(rl) == 1 && ab_jnum(rl->child, "lba", 0) == 12);
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 12, 13));
    CHECK(!strcmp(ab_jstr(res, "result", ""), "partial"));
    CHECK(ab_jbool(res, "complete", false));
    CHECK(ab_jnum(res, "recovered", 0) == 2);
    CHECK(!strcmp(ab_jstr(res, "readlong_sidecar", ""), SIDE));
    CHECK(sector_is_data(IMG, 10) && sector_is_data(IMG, 11) && sector_is_data(IMG, 12));
    CHECK(sector_is_zero(IMG, 13));
    CHECK(map_blocks(MAP, (blk[]){{0, 12 * 512, '+'}, {12 * 512, 2 * 512, '-'}, {14 * 512, (SECTORS - 14) * 512, '+'}}, 3));
    cJSON *side = load_json(SIDE);
    CHECK(side && cJSON_GetArraySize(side) == 1 && side->child && !strcmp(side->child->string, "12"));
    const cJSON *e = ab_jobj_get(side, "12");
    uint8_t ecc[4];
    char ehex[9];
    fake_ecc(12, 4, ecc);
    for (int i = 0; i < 4; i++) snprintf(ehex + 2 * i, 3, "%02x", ecc[i]);
    CHECK(!strcmp(ab_jstr(e, "ecc_hex", ""), ehex));
    CHECK(ab_jnum(e, "votes", 0) == 5);
    CHECK(ab_jnum(e, "unstable_bytes", 0) == 1);
    cJSON_Delete(side);
    CHECK(fake_readlong_count(&f, 12) == 5);
    // the no-retry pass used 0x21 with a 5 s cap, the retry pass 0x20
    CHECK(read_call(&f, 11, 1, 1, 5000));
    CHECK(read_call(&f, 11, 1, 0, 15000));
    // key order of the result (Python's dict)
    char *s = ab_json_print(res, 0);
    const char *keys[] = {"\"result\"", "\"image\"", "\"mapfile\"", "\"candidates\"", "\"recovered_no_retry\"",
                          "\"recovered_retry\"", "\"readlong\"", "\"still_bad\"", "\"readlong_skipped\"", "\"errors\"",
                          "\"complete\"", "\"readlong_sidecar\"", "\"map_saves\"", "\"recovered\"", "\"seconds\""};
    const char *at = s;
    for (size_t i = 0; s && i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *k = strstr(at, keys[i]);
        CHECK(k != NULL);
        if (k) at = k;
    }
    free(s);
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static void test_rescue_no_readlong(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    for (uint64_t l = 10; l < 14; l++) fake_bad(&f, l, FK_UNC);
    ab_rescue_opts o = ropts();
    o.readlong = false;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 10, 11, 12, 13));
    CHECK(!file_exists(SIDE));
    CHECK(count_op(&f, OP_READ_LONG) == 0);
    CHECK(!ab_jobj_get(res, "readlong_sidecar"));
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static void test_rescue_sidecar_merges(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    FILE *fp = fopen(SIDE, "wb");
    fputs("{\"40\": {\"lba\": 40, \"ecc_hex\": \"00\"}}", fp);
    fclose(fp);
    fake_bad(&f, 10, FK_UNC);
    ab_rescue_opts o = ropts();
    o.votes = 1;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    cJSON *side = load_json(SIDE);
    CHECK(side && cJSON_GetArraySize(side) == 2 && ab_jobj_get(side, "10") && ab_jobj_get(side, "40"));
    CHECK(side && side->child && !strcmp(side->child->string, "10"));       // sorted keys
    cJSON_Delete(side);
    // json.dump(indent=1, sort_keys=True) layout
    FILE *sf = fopen(SIDE, "rb");
    char text[4096] = {0};
    if (sf) { size_t n = fread(text, 1, sizeof(text) - 1, sf); text[n] = '\0'; fclose(sf); }
    CHECK(!strncmp(text, "{\n \"10\": {\n  \"ecc_hex\": \"", 25));
    CHECK(strstr(text, "\n },\n \"40\": {\n  \"ecc_hex\": \"00\",\n  \"lba\": 40\n }\n}") != NULL);
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

typedef struct { char seen[4]; int n; } peek_ctx;
static peek_ctx g_peek;
static int hook_peek(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f; (void)sense;
    if (op == OP_READ && ab_be32(a) == 11 && g_peek.n < 4) g_peek.seen[g_peek.n++] = map_status(MAP, 10);
    return 0;
}

static void test_rescue_checkpoints(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    memset(&g_peek, 0, sizeof(g_peek));
    f.hook = hook_peek;
    ab_rescue_opts o = ropts();
    o.readlong = false;
    o.save_interval_s = 0;               // saved after every recovered sector
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(g_peek.n == 1 && g_peek.seen[0] == '+');          // sector 10 already on disk as finished
    CHECK(ab_jnum(res, "map_saves", 0) >= 4);
    cJSON_Delete(res);
    rescue_teardown(d, &f);

    d = rescue_setup(&f);
    o = ropts();
    o.readlong = false;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(ab_jnum(res, "map_saves", 0) == 3);               // end of pass 1, pass 2, final
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static int hook_ctrl_c(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f; (void)sense;
    if (op == OP_READ && !(a[6] & 1)) ab_interrupt();        // first retry-pass read
    return 0;
}

static void test_rescue_ctrl_c(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    fake_bad(&f, 11, FK_NORETRY);
    fake_bad(&f, 12, FK_NORETRY);
    fake_bad(&f, 13, FK_NORETRY);
    f.hook = hook_ctrl_c;
    ab_rescue_opts o = ropts();
    o.readlong = false;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    ab_interrupt_reset();
    CHECK(!strcmp(ab_jstr(res, "result", ""), "interrupted"));
    CHECK(!ab_jbool(res, "complete", true));
    CHECK(ARR(ab_jobj_get(res, "recovered_no_retry"), 10));
    // the read in flight when ^C came completes (11 recovers with retries); then it stops
    CHECK(ARR(ab_jobj_get(res, "recovered_retry"), 11));
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 12, 13));
    CHECK(map_status(MAP, 10) == '+' && map_status(MAP, 11) == '+' && map_status(MAP, 12) == '-');
    CHECK(sector_is_data(IMG, 10));
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static int hook_boom(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f; (void)sense;
    if (op == OP_READ && !(a[6] & 1)) { ab_fail(AB_ERR_TRANSPORT, "device went away"); return AB_ERR_TRANSPORT; }
    return 0;
}

static void test_rescue_transport_error_saves_map(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    fake_bad(&f, 11, FK_NORETRY);
    fake_bad(&f, 12, FK_NORETRY);
    fake_bad(&f, 13, FK_NORETRY);
    f.hook = hook_boom;
    ab_rescue_opts o = ropts();
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_ERR_TRANSPORT && !res);
    CHECK(strstr(ab_errmsg(d), "went away") != NULL);
    CHECK(map_status(MAP, 10) == '+');                       // pass-1 recovery is in the map
    CHECK(map_status(MAP, 11) == '-');
    CHECK(sector_is_data(IMG, 10));
    rescue_teardown(d, &f);
}

static int hook_check_at_12(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]) {
    (void)f;
    if (op == OP_READ_LONG && ab_be32(a) == 12) {
        sense[0] = 4; sense[1] = 0; sense[2] = 0;
        ab_fail(AB_ERR_CHECK, "CHECK CONDITION");
        return AB_ERR_CHECK;
    }
    return 0;
}

static void test_rescue_error_in_readlong_saves_sidecar(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    for (uint64_t l = 10; l < 14; l++) fake_bad(&f, l, FK_UNC);
    f.hook = hook_check_at_12;
    ab_rescue_opts o = ropts();
    o.votes = 1;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_ERR_CHECK && !res);
    cJSON *side = load_json(SIDE);
    CHECK(side && cJSON_GetArraySize(side) == 2 && ab_jobj_get(side, "10") && ab_jobj_get(side, "11"));
    cJSON_Delete(side);
    CHECK(file_exists(MAP));
    CHECK(bad_sectors_are(MAP, (uint64_t[]){10, 11, 12, 13}, 4));
    rescue_teardown(d, &f);
}

static void test_rescue_api_error_per_sector(void) {
    // The drive is shorter than the map claims: LBAs 11..13 are beyond its
    // end, so every read of them is a JSON bad_arg.  The run must go on.
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    f.total = 11;
    ab_rescue_opts o = ropts();
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(ARR(ab_jobj_get(res, "recovered_no_retry"), 10));
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 11, 12, 13));
    const cJSON *errs = ab_jobj_get(res, "errors");
    CHECK(cJSON_GetArraySize(errs) == 9);
    bool lba_ok = true, msg_ok = true, p1 = false, p2 = false, p3 = false;
    for (const cJSON *x = errs ? errs->child : NULL; x; x = x->next) {
        double l = ab_jnum(x, "lba", 0);
        lba_ok &= l >= 11 && l <= 13;
        msg_ok &= strstr(ab_jstr(x, "error", ""), "beyond end of drive") != NULL;
        const char *p = ab_jstr(x, "pass", "");
        p1 |= !strcmp(p, "no-retry");
        p2 |= !strcmp(p, "retry");
        p3 |= !strcmp(p, "readlong");
    }
    CHECK(lba_ok && msg_ok && p1 && p2 && p3);
    CHECK(errs && !strcmp(ab_jstr(errs->child, "error", ""), "bad_arg: beyond end of drive"));
    CHECK(map_status(MAP, 10) == '+');
    CHECK(bad_sectors_are(MAP, (uint64_t[]){11, 12, 13}, 3));
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static void test_rescue_readlong_skipped_above_lba28(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    uint64_t big = ((uint64_t)1 << 28) + 5;
    f.total = big + 100;
    fake_bad(&f, big, FK_UNC);
    blank_image("t_rec_big.img", 1);
    write_map("t_rec_big.map", (blk[]){{0, big * 512, '+'}, {big * 512, 512, '-'}}, 2);
    ab_rescue_opts o = ropts();
    cJSON *res = NULL;
    CHECK(ab_rescue(d, "t_rec_big.img", "t_rec_big.map", &o, &res) == AB_OK);
    CHECK(ARR(ab_jobj_get(res, "still_bad"), (double)big));
    const cJSON *sk = ab_jobj_get(res, "readlong_skipped");
    CHECK(cJSON_GetArraySize(sk) == 1 && ab_jnum(sk->child, "lba", 0) == (double)big);
    CHECK(count_op(&f, OP_READ_LONG) == 0);
    CHECK(cJSON_GetArraySize(ab_jobj_get(res, "errors")) == 0);
    cJSON_Delete(res);
    remove("t_rec_big.img");
    remove("t_rec_big.map");
    remove("t_rec_big.img.readlong.json");
    rescue_teardown(d, &f);
}

static void test_rescue_untried_marked_bad(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    const char *mp = "t_rec_u.map";
    write_map(mp, (blk[]){{0, 10 * 512, '+'}, {10 * 512, 2 * 512, '?'}, {12 * 512, (SECTORS - 12) * 512, '+'}}, 3);
    fake_bad(&f, 11, FK_DEAD);
    ab_rescue_opts o = ropts();
    o.include_untried = true;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, mp, &o, &res) == AB_OK);
    CHECK(ARR(ab_jobj_get(res, "recovered_no_retry"), 10));
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 11));
    CHECK(map_status(mp, 10) == '+' && map_status(mp, 11) == '-');
    CHECK(bad_sectors_are(mp, (uint64_t[]){11}, 1));
    cJSON_Delete(res);
    remove(mp);
    rescue_teardown(d, &f);
}

static void test_rescue_time_budget(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    fake_bad(&f, 10, FK_UNC);
    fake_bad(&f, 11, FK_UNC);
    ab_rescue_opts o = ropts();
    o.time_budget_s = 1e-9;
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_OK);
    CHECK(!ab_jbool(res, "complete", true));
    CHECK(ARR(ab_jobj_get(res, "still_bad"), 10, 11, 12, 13));
    CHECK(count_op(&f, OP_READ) == 0 && count_op(&f, OP_READ_LONG) == 0);
    cJSON_Delete(res);
    rescue_teardown(d, &f);
}

static void test_rescue_bad_inputs(void) {
    fake_dev f;
    ab_dev *d = rescue_setup(&f);
    ab_rescue_opts o = ropts();
    cJSON *res = NULL;
    CHECK(ab_rescue(d, IMG, "t_rec_nonexistent.map", &o, &res) == AB_ERR_IO && !res);
    CHECK(ab_rescue(d, "t_rec_nonexistent.img", MAP, &o, &res) == AB_ERR_IO && !res);
    CHECK(f.ncalls == 0);
    o.votes = 65;
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_ERR_ARG);
    FILE *fp = fopen(SIDE, "wb");                // a corrupt sidecar is not overwritten
    fputs("[1, 2", fp);
    fclose(fp);
    fake_bad(&f, 10, FK_UNC);
    o = ropts();
    CHECK(ab_rescue(d, IMG, MAP, &o, &res) == AB_ERR_IO && !res);
    cJSON *j = load_json(SIDE);
    CHECK(j == NULL);
    rescue_teardown(d, &f);
}

int main(void) {
    test_hashes_parse();
    test_hashes_records();
    test_hashes_paging();
    test_hashes_ring_wraps();
    test_hashes_err_data_and_clear();
    test_hashes_compare();
    test_image_through_api();
    test_image_without_geometry();
    test_image_interrupt();
    test_image_transport_error_saves_map();
#if defined(__linux__)
    test_image_usb_disk();
#endif
    test_rescue_all_passes();
    test_rescue_no_readlong();
    test_rescue_sidecar_merges();
    test_rescue_checkpoints();
    test_rescue_ctrl_c();
    test_rescue_transport_error_saves_map();
    test_rescue_error_in_readlong_saves_sidecar();
    test_rescue_api_error_per_sector();
    test_rescue_readlong_skipped_above_lba28();
    test_rescue_untried_marked_bad();
    test_rescue_time_budget();
    test_rescue_bad_inputs();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("image/rescue/hashes tests passed\n");
    return 0;
}
