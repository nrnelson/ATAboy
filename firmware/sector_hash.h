#ifndef SECTOR_HASH_H
#define SECTOR_HASH_H

// SHA-256 of every 512-byte sector read from the drive (USB disk reads and
// API READ), kept in a ring of the most recent records.  A host can compare
// them with what it received, or keep them as a record of what the drive
// delivered.  Core 0 only (USB callbacks and the API).  See docs/API.md, op
// HASHES.
//
// The ring logic and the reply encoding have no SDK dependencies (host
// tests); the digest itself comes from sector_digest(), the RP2350 SHA-256
// block on the device (sha256_hw.c).

#include <stdint.h>
#include <stdbool.h>

#define HASH_RING_SIZE   1024
#define HASH_REC_LEN     44          // bytes per record in the HASHES reply
#define HASH_HDR_LEN     32          // HASHES reply header

// Where a sector was read
#define HASH_SRC_USB     1           // USB disk READ(10)
#define HASH_SRC_API     2           // API READ (0x22)

// Record flags
#define HASH_F_ERR_DATA  0x01        // data the drive offered with ERR (read-through-error)

typedef struct {
    uint32_t seq;                    // 1, 2, ... since boot or the last clear
    uint32_t lba;
    uint8_t  src;
    uint8_t  flags;
    uint8_t  digest[32];
} hash_rec_t;

// Hash 'count' consecutive sectors starting at 'lba' from 'buf'.
void     hash_sectors(uint32_t lba, const uint8_t *buf, uint32_t count, uint8_t src, uint8_t flags);
void     hash_clear(void);
uint32_t hash_newest(void);                  // seq of the newest record (0 = none yet)
const hash_rec_t *hash_get(uint32_t seq);    // NULL if not retained
bool     hash_enabled(void);                 // false if the digest self-test failed

// HASHES reply payload: header + whole records from 'start' (0 = oldest
// retained) that fit in 'cap' bytes.  Returns the payload length.
uint32_t hash_page(uint8_t *out, uint32_t cap, uint32_t start);

// Provided by the platform.
void     sector_digest(const uint8_t *data512, uint8_t out[32]);
bool     sector_digest_selftest(void);       // known-answer test, run before first use
uint32_t sector_hash_now_us(void);

#endif
