#ifndef FAKE_DEV_H
#define FAKE_DEV_H

// A model of the API firmware behind an ab_transport, for tests: READ,
// READ LONG, HASHES, INFO and STATUS over a simulated drive with bad sectors
// (after tests/test_host_lib.py FakeDevice).  Basic (16-byte) replies only:
// the extended signature gets CHECK CONDITION, like older firmware.

#include "internal.h"
#include <stdio.h>

typedef enum { FK_UNC = 1, FK_NORETRY, FK_DEAD, FK_TIMEOUT } fk_mode;

#define FK_MAX_BAD   64
#define FK_MAX_HASH  4096

typedef struct fake_dev fake_dev;
// Called before an op runs: return 0 to run it, or an ab_err for the
// transport to return (sense for AB_ERR_CHECK).
typedef int (*fk_hook)(fake_dev *f, uint8_t op, const uint8_t a[12], uint8_t sense[3]);

typedef struct { uint8_t op; uint8_t a[12]; } fk_call;
typedef struct { uint32_t seq, lba; uint8_t source, flags; uint8_t digest[32]; } fk_hash;

struct fake_dev {
    ab_transport tp;                 // first: the library sees this
    uint64_t total;
    unsigned ecc;
    bool mounted, geometry_valid;
    struct { uint64_t lba; fk_mode mode; } bad[FK_MAX_BAD];
    int nbad;
    uint64_t flaky[8];               // READ LONG byte 0 flips on every other read
    unsigned flaky_count[8];
    int nflaky;
    fk_call *calls;
    size_t ncalls, capcalls;
    fk_hook hook;
    void *hook_ctx;
    uint32_t uptime;
    fk_hash hashes[FK_MAX_HASH];
    size_t nhash;
    uint32_t hash_newest;
    unsigned hash_capacity;
};

void fake_init(fake_dev *f, uint64_t total);
void fake_free(fake_dev *f);
void fake_bad(fake_dev *f, uint64_t lba, fk_mode mode);
void fake_flaky(fake_dev *f, uint64_t lba);
unsigned fake_readlong_count(const fake_dev *f, uint64_t lba);
// An ab_dev talking to f (ab_close() it; f outlives it).
ab_dev *fake_open(fake_dev *f);
void fake_sector(uint64_t lba, uint8_t out[512]);
void fake_ecc(uint64_t lba, unsigned n, uint8_t *out);

#endif
