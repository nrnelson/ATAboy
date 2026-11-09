// Fuzz target: the CLI's JSON printer (ab_json_print, Python json.dumps
// layout) and the tolerant accessors over whatever cJSON parses from device
// replies.  Property: printing is stable, i.e. a printed tree that parses
// again prints the same text.
#include "ataboy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cJSON *j = cJSON_ParseWithLength((const char *)data, size);
    if (!j) return 0;
    (void)ab_jstr(j, "result", NULL);
    (void)ab_jnum(j, "total", 0);
    (void)ab_jbool(j, "mounted", false);
    (void)ab_jobj_get(ab_jobj_get(j, "config"), "read_timeout_ms");
    for (int indent = 0; indent <= 2; indent += 2) {
        char *s = ab_json_print(j, indent);
        if (!s) continue;
        cJSON *again = cJSON_Parse(s);         // fails for NaN/Infinity, lone surrogates
        if (again) {
            char *s2 = ab_json_print(again, indent);
            if (s2 && strcmp(s, s2) != 0) abort();
            free(s2);
            cJSON_Delete(again);
        }
        free(s);
    }
    char bits[64];
    if (size) {
        (void)ab_status_bits(data[0], bits);
        (void)ab_error_bits(data[size - 1], bits);
    }
    cJSON_Delete(j);
    return 0;
}
