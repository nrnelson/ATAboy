#ifndef RID_CACHE_H
#define RID_CACHE_H

// Run-once request ids (docs/API.md "Request ids").  A state-changing API
// request may carry a 32-bit id.  The reply to each of the last
// RID_CACHE_ENTRIES such requests is kept, so a host that lost a reply can
// send the same request again and get the stored reply instead of running
// the operation twice.  The same id with a different request is refused.
// Pure C (no SDK), so the host tests build it too.

#include <stdint.h>
#include <stdbool.h>

#define RID_CACHE_ENTRIES 4
#define RID_REPLY_MAX     4096

enum { RID_MISS = 0, RID_HIT = 1, RID_CONFLICT = 2 };

// Look up 'id' (nonzero).  On RID_HIT *reply/*len point at the stored reply.
int  rid_lookup(uint32_t id, const uint8_t req[16], const uint8_t **reply, uint32_t *len);
// Remember the reply to request 'req' with id 'id' (replaces the oldest entry).
// Replies longer than RID_REPLY_MAX are not stored (returns false).
bool rid_store(uint32_t id, const uint8_t req[16], const uint8_t *reply, uint32_t len);
void rid_clear(void);

#endif
