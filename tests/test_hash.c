// Host-side unit tests for the per-sector hash ring and its HASHES reply
// encoding (firmware/sector_hash.c).  The digest is a stand-in (the first 32
// data bytes); the real one is the RP2350 SHA-256 block.
//   cc -Wall -Wextra -I firmware -o /tmp/t tests/test_hash.c firmware/sector_hash.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include "sector_hash.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t now_us = 0;
static int selftests = 0;
void sector_digest(const uint8_t *data512, uint8_t out[32]) { memcpy(out, data512, 32); now_us += 10; }
bool sector_digest_selftest(void) { selftests++; return true; }
uint32_t sector_hash_now_us(void) { return now_us; }

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)((p[0] << 8) | p[1]); }

static uint8_t secs[8 * 512];
static uint8_t page[4096];

static void fill(uint32_t lba0, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { memset(secs + i * 512, 0, 512); uint32_t v = lba0 + i; memcpy(secs + i * 512, &v, 4); }
}

int main(void) {
    // empty ring
    uint32_t len = hash_page(page, sizeof(page), 0);
    CHECK(len == HASH_HDR_LEN);
    CHECK(be32(page) == 0 && be32(page + 4) == 0 && be16(page + 12) == 0);
    CHECK(be16(page + 14) == HASH_REC_LEN && be16(page + 16) == HASH_RING_SIZE && page[18] == 1);
    CHECK(selftests == 1);

    // a few sectors from both sources
    fill(100, 8);
    hash_sectors(100, secs, 8, HASH_SRC_USB, 0);
    hash_sectors(103, secs + 3 * 512, 1, HASH_SRC_API, HASH_F_ERR_DATA);
    hash_sectors(5, secs, 0, HASH_SRC_USB, 0);          // nothing
    CHECK(hash_newest() == 9);
    len = hash_page(page, sizeof(page), 0);
    CHECK(len == HASH_HDR_LEN + 9 * HASH_REC_LEN);
    CHECK(be32(page) == 9 && be32(page + 4) == 1 && be32(page + 8) == 10 && be16(page + 12) == 9);
    CHECK(be32(page + 20) == 0 && be32(page + 24) == 90);   // busy_us
    const uint8_t *r = page + HASH_HDR_LEN + 8 * HASH_REC_LEN;
    CHECK(be32(r) == 9 && be32(r + 4) == 103 && r[8] == HASH_SRC_API && r[9] == HASH_F_ERR_DATA);
    uint32_t v; memcpy(&v, r + 12, 4);
    CHECK(v == 103);
    r = page + HASH_HDR_LEN;
    CHECK(be32(r) == 1 && be32(r + 4) == 100 && r[8] == HASH_SRC_USB && r[9] == 0);

    // paging: a small allocation holds whole records only
    len = hash_page(page, HASH_HDR_LEN + 2 * HASH_REC_LEN + 10, 4);
    CHECK(len == HASH_HDR_LEN + 2 * HASH_REC_LEN);
    CHECK(be16(page + 12) == 2 && be32(page + 8) == 6 && be32(page + HASH_HDR_LEN) == 4);
    // asking past the newest returns nothing and keeps the cursor
    len = hash_page(page, sizeof(page), 50);
    CHECK(len == HASH_HDR_LEN && be32(page + 8) == 50);

    // wrap: only the last HASH_RING_SIZE are retained
    for (uint32_t i = 0; i < HASH_RING_SIZE; i += 8) { fill(1000 + i, 8); hash_sectors(1000 + i, secs, 8, HASH_SRC_USB, 0); }
    CHECK(hash_newest() == 9 + HASH_RING_SIZE);
    CHECK(hash_get(9) == NULL && hash_get(10) != NULL && hash_get(10)->lba == 1000);
    len = hash_page(page, sizeof(page), 1);
    CHECK(be32(page + 4) == 10 && be32(page + HASH_HDR_LEN) == 10);
    uint32_t per = (sizeof(page) - HASH_HDR_LEN) / HASH_REC_LEN;
    CHECK(be16(page + 12) == per && be32(page + 8) == 10 + per);

    // clear
    hash_clear();
    CHECK(hash_newest() == 0 && hash_get(1) == NULL);
    len = hash_page(page, sizeof(page), 0);
    CHECK(len == HASH_HDR_LEN && be32(page + 24) == 0);
    CHECK(selftests == 1);
    CHECK(hash_page(page, HASH_HDR_LEN - 1, 0) == 0);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all sector_hash tests passed\n");
    return 0;
}
