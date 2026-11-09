// Stand-alone driver for the fuzz targets when they are not linked with
// libFuzzer (ATABOY_FUZZ=OFF, e.g. gcc in CI):
//
//   fuzz_x FILE...                         run each input once (corpus replay)
//   fuzz_x -mutate=N [-seed=S] FILE...     also N random mutations of them
//
// With -mutate the input under test is written to $FUZZ_LAST (if set) first,
// so the case that made a sanitizer abort can be kept.
#ifndef AB_FUZZ_MAIN_H
#define AB_FUZZ_MAIN_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#if !defined(AB_LIBFUZZER)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_INPUT (1u << 20)

static uint64_t fz_state = 88172645463325252ull;
static uint32_t fz_rand(void) {
    fz_state ^= fz_state << 13;
    fz_state ^= fz_state >> 7;
    fz_state ^= fz_state << 17;
    return (uint32_t)(fz_state >> 16);
}

static uint8_t *fz_load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(FUZZ_MAX_INPUT);
    *n = b ? fread(b, 1, FUZZ_MAX_INPUT, f) : 0;
    fclose(f);
    return b;
}

static size_t fz_mutate(uint8_t *d, size_t n, size_t cap) {
    static const uint8_t magic[] = {0x00, 0xFF, 0x7F, 0x80, 0xE5, 0x0F, 0x10, 0x2E, 0x05, 0x20, 0x29, 0x55, 0xAA, 0xA5};
    int k = 1 + (int)(fz_rand() % 16);
    for (int i = 0; i < k; i++) {
        uint32_t r = fz_rand() % 100;
        if (n == 0) { d[0] = (uint8_t)fz_rand(); n = 1; continue; }
        size_t p = fz_rand() % n;
        if (r < 30) d[p] = (uint8_t)fz_rand();
        else if (r < 50) d[p] ^= (uint8_t)(1u << (fz_rand() % 8));
        else if (r < 70) d[p] = magic[fz_rand() % sizeof(magic)];
        else if (r < 80 && n >= 4) {                      // little/big-endian word
            uint32_t v = fz_rand() % 4 == 0 ? 0xFFFFFFFFu : fz_rand() % 3 == 0 ? fz_rand() % 70000 : fz_rand();
            size_t q = fz_rand() % (n - 3);
            memcpy(d + q, &v, 4);
        } else if (r < 90) {                              // copy a block within
            size_t q = fz_rand() % n, len = 1 + fz_rand() % 64;
            if (p + len > n) len = n - p;
            if (q + len > n) len = n - q;
            memmove(d + p, d + q, len);
        } else if (r < 95) {
            n = 1 + fz_rand() % n;                        // truncate
        } else if (n < cap) {
            size_t add = 1 + fz_rand() % 512;
            if (n + add > cap) add = cap - n;
            memset(d + n, (int)(fz_rand() & 0xFF), add);  // extend
            n += add;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    long mutate = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (!strncmp(argv[first], "-mutate=", 8)) mutate = atol(argv[first] + 8);
        else if (!strncmp(argv[first], "-seed=", 6)) fz_state ^= (uint64_t)strtoull(argv[first] + 6, NULL, 0) * 0x9E3779B97F4A7C15ull;
        else { fprintf(stderr, "unknown option %s\n", argv[first]); return 2; }
    }
    int nin = argc - first, runs = 0;
    if (nin <= 0) {                                       // no corpus: still exercise the empty input
        LLVMFuzzerTestOneInput((const uint8_t *)"", 0);
        printf("ran 1 input\n");
        return 0;
    }
    for (int i = first; i < argc; i++) {
        size_t n;
        uint8_t *b = fz_load(argv[i], &n);
        if (!b) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        LLVMFuzzerTestOneInput(b, n);
        free(b);
        runs++;
    }
    const char *last = getenv("FUZZ_LAST");
    uint8_t *work = malloc(FUZZ_MAX_INPUT);
    for (long m = 0; m < mutate && work; m++) {
        size_t n;
        uint8_t *b = fz_load(argv[first + (int)(fz_rand() % (uint32_t)nin)], &n);
        if (!b) break;
        memcpy(work, b, n);
        free(b);
        n = fz_mutate(work, n, FUZZ_MAX_INPUT);
        if (last) {
            FILE *f = fopen(last, "wb");
            if (f) { fwrite(work, 1, n, f); fclose(f); }
        }
        LLVMFuzzerTestOneInput(work, n);
        runs++;
    }
    free(work);
    printf("ran %d inputs\n", runs);
    return 0;
}
#endif
#endif
