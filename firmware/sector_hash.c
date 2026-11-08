// Ring of per-sector SHA-256 records (see sector_hash.h).

#include "sector_hash.h"
#include <string.h>

static hash_rec_t ring[HASH_RING_SIZE];
static uint32_t newest;             // records made since boot/clear = seq of the newest
static uint64_t busy_us;            // time spent hashing since boot/clear
static int8_t   state = -1;         // -1 not tested yet, 0 self-test failed (off), 1 on

bool hash_enabled(void) {
    if (state < 0) state = sector_digest_selftest() ? 1 : 0;
    return state == 1;
}

void hash_clear(void) { newest = 0; busy_us = 0; }
uint32_t hash_newest(void) { return newest; }

static uint32_t oldest(void) {
    if (!newest) return 0;
    return newest > HASH_RING_SIZE ? newest - HASH_RING_SIZE + 1 : 1;
}

const hash_rec_t *hash_get(uint32_t seq) {
    if (!newest || seq < oldest() || seq > newest) return NULL;
    return &ring[(seq - 1) % HASH_RING_SIZE];
}

void hash_sectors(uint32_t lba, const uint8_t *buf, uint32_t count, uint8_t src, uint8_t flags) {
    if (!count || !hash_enabled()) return;
    uint32_t t0 = sector_hash_now_us();
    for (uint32_t i = 0; i < count; i++) {
        hash_rec_t *r = &ring[newest % HASH_RING_SIZE];
        r->seq = ++newest;
        r->lba = lba + i;
        r->src = src;
        r->flags = flags;
        sector_digest(buf + i * 512, r->digest);
    }
    busy_us += sector_hash_now_us() - t0;
}

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }

uint32_t hash_page(uint8_t *out, uint32_t cap, uint32_t start) {
    if (cap < HASH_HDR_LEN) return 0;
    uint32_t first = oldest();
    if (start < first) start = first;
    uint32_t n = 0, seq = start;
    if (first) {
        for (; seq <= newest && HASH_HDR_LEN + (n + 1) * HASH_REC_LEN <= cap; seq++, n++) {
            const hash_rec_t *r = hash_get(seq);
            uint8_t *p = out + HASH_HDR_LEN + n * HASH_REC_LEN;
            be32(p, r->seq);
            be32(p + 4, r->lba);
            p[8] = r->src;
            p[9] = r->flags;
            p[10] = p[11] = 0;
            memcpy(p + 12, r->digest, 32);
        }
    }
    memset(out, 0, HASH_HDR_LEN);
    be32(out, newest);
    be32(out + 4, first);
    be32(out + 8, seq);                 // ask for this next
    be16(out + 12, n);
    be16(out + 14, HASH_REC_LEN);
    be16(out + 16, HASH_RING_SIZE);
    out[18] = hash_enabled() ? 1 : 0;
    be32(out + 20, (uint32_t)(busy_us >> 32));
    be32(out + 24, (uint32_t)busy_us);
    return HASH_HDR_LEN + n * HASH_REC_LEN;
}
