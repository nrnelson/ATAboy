// libFuzzer target for the ddrescue mapfile parser (untrusted input).
//   clang -fsanitize=fuzzer,address,undefined ... (cmake -DATABOY_FUZZ=ON)
// Every accepted map must satisfy the parser's invariants, survive mark()
// and summary(), and give consistent answers from the query functions.
#include "ataboy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ab_mapfile *m = NULL;
    if (ab_mapfile_parse((const char *)data, size, "fuzz.map", &m) != AB_OK) {
        if (m) abort();                     // failure must not hand out a map
        return 0;
    }
    size_t n;
    const ab_mapblock *b = ab_mapfile_blocks(m, &n);
    uint64_t prev_end = 0;
    for (size_t i = 0; i < n; i++) {        // sorted, non-overlapping, non-empty
        if (b[i].size == 0 || b[i].pos < prev_end) abort();
        if (!strchr("?*/-+", b[i].status)) abort();
        prev_end = b[i].pos + b[i].size;
        if (ab_mapfile_status_at(m, b[i].pos / 512 + (b[i].pos % 512 ? 1 : 0)) == '\0' && b[i].size >= 1024) abort();
    }
    uint64_t *lbas = NULL;
    size_t nl = 0;
    if (ab_mapfile_bad_sectors(m, 1000, true, &lbas, &nl) == AB_OK && nl > 1000) abort();
    free(lbas);
    cJSON_Delete(ab_mapfile_summary(m));
    // mark a few ranges derived from the input; the invariants must hold after
    for (size_t i = 0; i + 3 <= size && i < 24; i += 3) {
        uint64_t lba = (prev_end / 512) * data[i] / 255;
        uint64_t cnt = (uint64_t)data[i + 1] + 1;
        const char st = "?*/-+"[data[i + 2] % 5];
        if (ab_mapfile_mark(m, lba, cnt, st) != AB_OK) continue;
        b = ab_mapfile_blocks(m, &n);
        uint64_t pe = 0;
        for (size_t k = 0; k < n; k++) {
            if (b[k].size == 0 || b[k].pos < pe) abort();
            if (k && b[k - 1].status == b[k].status && b[k - 1].pos + b[k - 1].size == b[k].pos) abort();
            pe = b[k].pos + b[k].size;
        }
        if (pe != prev_end) abort();        // marking never grows or shrinks the map
        char got = ab_mapfile_status_at(m, lba);
        if (got != '\0' && got != st) abort();
    }
    ab_mapfile_free(m);
    return 0;
}

#ifdef FUZZ_STANDALONE
// Without libFuzzer: run the inputs named on the command line.
#include <stdio.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        static uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
    }
    return 0;
}
#endif
