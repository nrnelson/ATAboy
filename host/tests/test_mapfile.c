// ddrescue mapfile: round trip, mark/merge, queries, malformed input.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define HDR "# Mapfile. Created by GNU ddrescue version 1.27\n# current_pos  current_status  current_pass\n" \
            "0x00000000     +               1\n#      pos        size  status\n"

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool blocks_are(const ab_mapfile *m, const ab_mapblock *want, size_t n) {
    size_t got;
    const ab_mapblock *b = ab_mapfile_blocks(m, &got);
    if (got != n) { printf("  %zu blocks, want %zu\n", got, n); return false; }
    for (size_t i = 0; i < n; i++)
        if (b[i].pos != want[i].pos || b[i].size != want[i].size || b[i].status != want[i].status) {
            printf("  block %zu: 0x%llx 0x%llx %c, want 0x%llx 0x%llx %c\n", i, (unsigned long long)b[i].pos,
                   (unsigned long long)b[i].size, b[i].status, (unsigned long long)want[i].pos,
                   (unsigned long long)want[i].size, want[i].status);
            return false;
        }
    return true;
}

static bool lbas_are(const uint64_t *v, size_t n, const uint64_t *want, size_t wn) {
    if (n != wn) return false;
    for (size_t i = 0; i < n; i++) if (v[i] != want[i]) return false;
    return true;
}

static void test_round_trip(void) {
    const char *p = "t_map_rt.map";
    write_file(p, HDR "0x00000000  0x00002000  +\n0x00002000  0x00000400  -\n0x00002400  0x00000200  *\n"
                      "0x00002600  0x00000200  /\n0x00002800  0x00000800  ?\n0x00003000  0x00001000  +\n");
    ab_mapfile *m = NULL;
    CHECK(ab_mapfile_load(p, &m) == AB_OK);
    if (!m) return;
    CHECK(ab_mapfile_header_lines(m) == 2);
    CHECK(!strcmp(ab_mapfile_header_line(m, 0), "# Mapfile. Created by GNU ddrescue version 1.27"));
    CHECK(!strcmp(ab_mapfile_cur_line(m), "0x00000000     +               1"));
    uint64_t *v;
    size_t n;
    CHECK(ab_mapfile_bad_sectors(m, 0, false, &v, &n) == AB_OK);
    CHECK(lbas_are(v, n, (uint64_t[]){16, 17, 18, 19}, 4));
    free(v);
    CHECK(ab_mapfile_bad_sectors(m, 0, true, &v, &n) == AB_OK);
    CHECK(lbas_are(v, n, (uint64_t[]){16, 17, 18, 19, 20, 21, 22, 23}, 8));
    free(v);
    CHECK(ab_mapfile_bad_sectors(m, 2, false, &v, &n) == AB_OK);
    CHECK(lbas_are(v, n, (uint64_t[]){16, 17}, 2));
    free(v);
    CHECK(ab_mapfile_status_at(m, 20) == '?');
    CHECK(ab_mapfile_status_at(m, 10000) == '\0');
    cJSON *s = ab_mapfile_summary(m);
    char *js = ab_json_print(s, 0);
    CHECK(!strcmp(js, "{\"+\": 12288, \"-\": 1024, \"*\": 512, \"/\": 512, \"?\": 2048}"));
    free(js);
    cJSON_Delete(s);
    ab_mapfile_mark(m, 16, 1, '+');                   // merges with the '+' block before it
    ab_mapfile_mark(m, 17, 1, '+');
    size_t nb;
    const ab_mapblock *b = ab_mapfile_blocks(m, &nb);
    CHECK(b[0].pos == 0 && b[0].size == 0x2400 && b[0].status == '+');
    ab_mapfile_mark(m, 21, 1, '-');                   // splits '?'
    ab_mapfile_mark(m, 18, 2, '+');
    ab_mapfile_mark(m, 20, 4, '+');                   // everything recovered: one block
    CHECK(blocks_are(m, (ab_mapblock[]){{0, 0x4000, '+'}}, 1));
    CHECK(ab_mapfile_save(m) == AB_OK);
    ab_mapfile *m2 = NULL;
    CHECK(ab_mapfile_load(p, &m2) == AB_OK);
    if (m2) {
        CHECK(blocks_are(m2, (ab_mapblock[]){{0, 0x4000, '+'}}, 1));
        CHECK(ab_mapfile_header_lines(m2) == 2);
        CHECK(!strcmp(ab_mapfile_cur_line(m2), ab_mapfile_cur_line(m)));
        ab_mapfile_free(m2);
    }
    char *text = read_file(p);
    CHECK(text && !strcmp(text, HDR "0x00000000  0x00004000  +\n"));
    FILE *t = fopen("t_map_rt.map.tmp", "rb");
    CHECK(t == NULL);
    if (t) fclose(t);
    ab_mapfile_free(m);
    remove(p);
}

static void test_split_middle(void) {
    ab_mapfile *m = NULL;
    const char *txt = HDR "0x00000000  0x00001000  ?\n";
    CHECK(ab_mapfile_parse(txt, strlen(txt), "t_map_split.map", &m) == AB_OK);
    if (!m) return;
    ab_mapfile_mark(m, 3, 2, '-');
    CHECK(blocks_are(m, (ab_mapblock[]){{0, 0x600, '?'}, {0x600, 0x400, '-'}, {0xA00, 0x600, '?'}}, 3));
    CHECK(ab_mapfile_save(m) == AB_OK);
    ab_mapfile *m2 = NULL;
    CHECK(ab_mapfile_load("t_map_split.map", &m2) == AB_OK);
    if (m2) CHECK(blocks_are(m2, (ab_mapblock[]){{0, 0x600, '?'}, {0x600, 0x400, '-'}, {0xA00, 0x600, '?'}}, 3));
    ab_mapfile_free(m2);
    // mark spanning several blocks, a gap, and the map's end
    ab_mapfile_mark(m, 2, 100, '+');
    CHECK(blocks_are(m, (ab_mapblock[]){{0, 0x400, '?'}, {0x400, 0xC00, '+'}}, 2));
    ab_mapfile_free(m);
    remove("t_map_split.map");
}

static void test_gaps_and_merge_like_python(void) {
    // Two adjacent equal blocks stay as loaded until the first mark, which
    // (like Python's) merges the whole map.  Gaps are never filled.
    const char *txt = HDR "0x0000  0x0400  +\n0x0400  0x0400  +\n0x1000  0x0400  -\n";
    ab_mapfile *m = NULL;
    CHECK(ab_mapfile_parse(txt, strlen(txt), "x", &m) == AB_OK);
    if (!m) return;
    size_t nb;
    ab_mapfile_blocks(m, &nb);
    CHECK(nb == 3);
    CHECK(ab_mapfile_status_at(m, 5) == '\0');          // in the gap
    ab_mapfile_mark(m, 4, 6, '?');                       // covers the gap: only mapped parts change
    CHECK(blocks_are(m, (ab_mapblock[]){{0, 0x800, '+'}, {0x1000, 0x400, '?'}}, 2));
    ab_mapfile_free(m);
}

static void test_create(void) {
    ab_mapfile *m = NULL;
    CHECK(ab_mapfile_create("t_map_new.map", (uint64_t)1 << 42, &m) == AB_OK);   // 4 TB
    if (!m) return;
    CHECK(blocks_are(m, (ab_mapblock[]){{0, (uint64_t)1 << 42, '?'}}, 1));
    ab_mapfile_mark(m, ((uint64_t)1 << 32) - 1, 2, '-');
    CHECK(ab_mapfile_status_at(m, ((uint64_t)1 << 32)) == '-');
    CHECK(ab_mapfile_save(m) == AB_OK);
    char *text = read_file("t_map_new.map");
    CHECK(text && !strcmp(text, "# Mapfile. Created by ataboy " ATABOY_LIB_VERSION "\n"
                                "0x00000000     ?               1\n#      pos        size  status\n"
                                "0x00000000  0x1FFFFFFFE00  ?\n0x1FFFFFFFE00  0x00000400  -\n"
                                "0x20000000200  0x1FFFFFFFE00  ?\n"));
    ab_mapfile_free(m);
    remove("t_map_new.map");
}

static void test_line_endings_and_numbers(void) {
    const char *txt = "# hdr\r\n\r\n0 ? 1\r\n# comment after the status line is dropped\r\n"
                      "0  512  +\r1024 0x200 -\n0o3000 0b1000000000 /   extra fields ignored\n";
    ab_mapfile *m = NULL;
    CHECK(ab_mapfile_parse(txt, strlen(txt), "x", &m) == AB_OK);
    if (!m) return;
    CHECK(ab_mapfile_header_lines(m) == 2);
    CHECK(!strcmp(ab_mapfile_header_line(m, 1), ""));
    CHECK(!strcmp(ab_mapfile_cur_line(m), "0 ? 1"));
    CHECK(blocks_are(m, (ab_mapblock[]){{0, 512, '+'}, {1024, 512, '-'}, {1536, 512, '/'}}, 3));
    ab_mapfile_free(m);
}

static void test_malformed(void) {
    static const char *const bad[] = {
        "",                                               // empty
        "# only comments\n",
        "\n\n   \n",
        HDR "0x0 0x200\n",                                // missing status
        HDR "0x0 0x200 x\n",                              // bad status
        HDR "0x0 0x200 ++\n",
        HDR "zz 0x200 +\n",                               // bad numbers
        HDR "0x 0x200 +\n",
        HDR "0x0 -0x200 +\n",
        HDR "010 0x200 +\n",                              // Python int("010", 0) refuses
        HDR "0x0 0x0 +\n",                                // empty block
        HDR "0x0 0x10000000000000000 +\n",                // > 2^64
        HDR "0xFFFFFFFFFFFFFE00 0x200 +\n",               // beyond 2^62
        HDR "0x3FFFFFFFFFFFFE00 0x400 +\n",
        HDR "0x400 0x200 +\n0x0 0x200 -\n",               // out of order
        HDR "0x0 0x400 +\n0x200 0x200 -\n",               // overlapping
        HDR "0x0 0x2\0" "00 +\n",                         // NUL byte (length below includes it)
    };
    static const size_t len_extra[sizeof(bad) / sizeof(bad[0])] = {[16] = 6};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ab_mapfile *m = NULL;
        size_t n = strlen(bad[i]) + len_extra[i];
        int rc = ab_mapfile_parse(bad[i], n, "bad.map", &m);
        if (rc != AB_ERR_IO || m) { printf("  malformed input %zu accepted (rc %d)\n", i, rc); failures++; }
        ab_mapfile_free(m);
    }
    ab_mapfile *m = NULL;
    CHECK(ab_mapfile_parse(HDR "0x0 0x200 x\n", strlen(HDR) + 12, "bad.map", &m) == AB_ERR_IO);
    CHECK(strstr(ab_last_error(), "bad.map:5:") != NULL);
    CHECK(ab_mapfile_load("t_map_does_not_exist.map", &m) == AB_ERR_IO && !m);
    write_file("t_map_x.map", "# only comments\n");
    CHECK(ab_mapfile_load("t_map_x.map", &m) == AB_ERR_IO);
    CHECK(strstr(ab_last_error(), "not a ddrescue mapfile") != NULL);
    remove("t_map_x.map");
}

static void test_big_fragmented(void) {
    // 400k alternating blocks (a badly damaged 2 TB drive): marks stay cheap.
    enum { N = 400000 };
    size_t cap = (size_t)N * 40 + 256;
    char *txt = malloc(cap);
    size_t len = (size_t)snprintf(txt, cap, "%s", HDR);
    uint64_t pos = 0;
    for (int i = 0; i < N; i++) {
        uint64_t size = (i & 1) ? 512 : 512 * 1000;
        len += (size_t)snprintf(txt + len, cap - len, "0x%llX 0x%llX %c\n", (unsigned long long)pos,
                                (unsigned long long)size, (i & 1) ? '-' : '+');
        pos += size;
    }
    ab_mapfile *m = NULL;
    uint64_t t0 = ab_now_ms();
    CHECK(ab_mapfile_parse(txt, len, "big.map", &m) == AB_OK);
    free(txt);
    if (!m) return;
    uint64_t *v;
    size_t n;
    CHECK(ab_mapfile_bad_sectors(m, 0, false, &v, &n) == AB_OK && n == N / 2);
    // recover every bad sector: each mark merges three blocks into one
    for (size_t i = 0; i < n; i++) CHECK(ab_mapfile_mark(m, v[i], 1, '+') == AB_OK);
    free(v);
    size_t nb;
    ab_mapfile_blocks(m, &nb);
    CHECK(nb == 1);
    CHECK(ab_mapfile_status_at(m, pos / 512 - 1) == '+');
    uint64_t ms = ab_now_ms() - t0;
    printf("  400k-block map parsed and fully marked in %llu ms\n", (unsigned long long)ms);
    CHECK(ms < 20000);
    // too many sectors to list at once is refused, not attempted
    CHECK(ab_mapfile_mark(m, 0, pos / 512, '?') == AB_OK);
    if (pos / 512 > ((uint64_t)1 << 28)) CHECK(ab_mapfile_bad_sectors(m, 0, true, &v, &n) == AB_ERR_NOMEM);
    CHECK(ab_mapfile_bad_sectors(m, 10, true, &v, &n) == AB_OK && n == 10);
    free(v);
    ab_mapfile_free(m);
}

int main(void) {
    test_round_trip();
    test_split_middle();
    test_gaps_and_merge_like_python();
    test_create();
    test_line_endings_and_numbers();
    test_malformed();
    test_big_fragmented();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("mapfile tests passed\n");
    return 0;
}
