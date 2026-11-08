#include "rid_cache.h"
#include <string.h>

typedef struct {
    uint32_t id;               // 0 = empty
    uint32_t len;
    uint8_t  req[16];
    uint8_t  reply[RID_REPLY_MAX];
} rid_entry_t;

static rid_entry_t entries[RID_CACHE_ENTRIES];
static uint32_t next_slot;

int rid_lookup(uint32_t id, const uint8_t req[16], const uint8_t **reply, uint32_t *len) {
    if (id == 0) return RID_MISS;
    for (int i = 0; i < RID_CACHE_ENTRIES; i++) {
        rid_entry_t *e = &entries[i];
        if (e->id != id) continue;
        if (memcmp(e->req, req, 16) != 0) return RID_CONFLICT;
        *reply = e->reply;
        *len = e->len;
        return RID_HIT;
    }
    return RID_MISS;
}

bool rid_store(uint32_t id, const uint8_t req[16], const uint8_t *reply, uint32_t len) {
    if (id == 0 || len > RID_REPLY_MAX) return false;
    rid_entry_t *e = &entries[next_slot];
    next_slot = (next_slot + 1) % RID_CACHE_ENTRIES;
    e->id = id;
    e->len = len;
    memcpy(e->req, req, 16);
    memcpy(e->reply, reply, len);
    return true;
}

void rid_clear(void) {
    memset(entries, 0, sizeof(entries));
    next_slot = 0;
}
