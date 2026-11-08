// sector_digest() on the RP2350 SHA-256 block.  A sector is always 512 bytes,
// so the message padding is one constant block.  Costs roughly 9 x 16 word
// writes plus 9 block compressions per sector; the measured time is in the
// HASHES reply (busy_us / records).

#include "sector_hash.h"
#include "hardware/sha256.h"
#include "hardware/resets.h"
#include "pico/time.h"
#include <string.h>

// 0x80, zeros, then the message length in bits (4096) as a 64-bit big-endian number
static const uint8_t pad[64] = { [0] = 0x80, [62] = 0x10, [63] = 0x00 };

static inline void put_words(const uint8_t *p, uint32_t nbytes) {
    for (uint32_t i = 0; i < nbytes; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);              // the buffer may not be word aligned
        sha256_wait_ready_blocking();
        sha256_put_word(w);
    }
}

void sector_digest(const uint8_t *data512, uint8_t out[32]) {
    sha256_err_not_ready_clear();
    sha256_set_bswap(true);                // byte stream in memory order -> big-endian words
    sha256_start();
    put_words(data512, 512);
    put_words(pad, sizeof(pad));
    sha256_wait_valid_blocking();
    sha256_result_t r;
    sha256_get_result(&r, SHA256_BIG_ENDIAN);
    memcpy(out, r.bytes, 32);
}

uint32_t sector_hash_now_us(void) { return time_us_32(); }

// Known-answer test (SHA-256 of a zero sector), run before the first use.
bool sector_digest_selftest(void) {
    unreset_block_mask_wait_blocking(RESETS_RESET_SHA256_BITS);
    static const uint8_t zero_sector_sha[32] = {
        0x07, 0x6a, 0x27, 0xc7, 0x9e, 0x5a, 0xce, 0x2a, 0x3d, 0x47, 0xf9, 0xdd, 0x2e, 0x83, 0xe4, 0xff,
        0x6e, 0xa8, 0x87, 0x2b, 0x3c, 0x22, 0x18, 0xf6, 0x6c, 0x92, 0xb8, 0x9b, 0x55, 0xf3, 0x65, 0x60,
    };
    static uint8_t sec[513];
    uint8_t d[32];
    memset(sec, 0, sizeof(sec));
    sector_digest(sec + 1, d);             // unaligned on purpose: the API READ buffer may be
    return memcmp(d, zero_sector_sha, 32) == 0;
}
