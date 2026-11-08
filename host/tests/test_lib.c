// libataboy unit tests with a fake transport (no hardware).
#include "internal.h"
#include "api_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    cJSON *j = cJSON_Parse("{\"a\":1,\"b\":[1,2.5,\"x\\u00e9\"],\"c\":{},\"d\":[],\"e\":null,\"f\":true}");
    char *s = ab_json_print(j, 2);
    CHECK(s && !strcmp(s, "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2.5,\n    \"x\\u00e9\"\n  ],\n  \"c\": {},\n  \"d\": [],\n  \"e\": null,\n  \"f\": true\n}"));
    free(s);
    s = ab_json_print(j, 0);
    CHECK(s && !strcmp(s, "{\"a\": 1, \"b\": [1, 2.5, \"x\\u00e9\"], \"c\": {}, \"d\": [], \"e\": null, \"f\": true}"));
    free(s);
    cJSON_Delete(j);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all libataboy tests passed\n");
    return 0;
}
