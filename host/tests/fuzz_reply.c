// Fuzz target: API reply parsing (header, extended header, JSON payload), the
// SAT sense / ATA Status Return decoder and the IDENTIFY decoder.
#include "internal.h"
#include "fuzz_main.h"
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ab_reply r;
    memset(&r, 0, sizeof(r));
    r.raw_len = size < sizeof(r.raw) ? size : sizeof(r.raw);
    memcpy(r.raw, data, r.raw_len);
    if (ab_parse_reply(&r) == AB_OK && r.json) {
        char *s = ab_json_print(r.json, 2);
        free(s);
    }
    ab_reply_free(&r);

    cJSON *j = ab_ata_sense_result(0xEC, data, size < 252 ? size : 252);
    char *s = ab_json_print(j, 0);
    free(s);
    cJSON_Delete(j);

    if (size >= 512) {
        cJSON *id = ab_decode_identify(data);
        s = ab_json_print(id, 2);
        free(s);
        cJSON_Delete(id);
    }
    return 0;
}
