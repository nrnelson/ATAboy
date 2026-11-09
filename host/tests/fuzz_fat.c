// Fuzz target: whatfile on an arbitrary disk image (FAT, MBR, EBR parsing).
#include "ataboy.h"
#include "fuzz_main.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20)) return 0;
    // Every sector of the image (up to 2048), the two after it, and a few far
    // away or invalid ones.
    int64_t lbas[2048 + 8];
    size_t n = 0, sectors = size / 512 + 2;
    for (size_t i = 0; i < sectors && n < 2048; i++) lbas[n++] = (int64_t)i;
    lbas[n++] = -1;
    lbas[n++] = 0xFFFFFFFFll;
    lbas[n++] = 1ll << 40;
    lbas[n++] = 0;                         // a repeat
    cJSON *out = NULL;
    if (ab_whatfile_mem(data, size, "fuzz.img", lbas, n, &out) == AB_OK) {
        char *s = ab_json_print(out, 2);
        free(s);
    }
    cJSON_Delete(out);
    return 0;
}
