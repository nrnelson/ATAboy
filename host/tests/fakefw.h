#ifndef FAKEFW_H
#define FAKEFW_H

// A small model of the API firmware's request handling for the host tests:
// api_dispatch() (signature check, extended header, run-once request ids via
// the firmware's own rid_cache.c) in front of a handful of JSON ops.  Byte
// layout as firmware/api.c builds it.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool        ext;                 // firmware with the extended header and request ids
    uint32_t    boot_id, uptime;
    uint32_t    state_counter;       // bumped by every executed request-id op
    bool        mounted, write_protect, no_retry;
    uint32_t    read_timeout_ms;
    unsigned    device;              // 0 master, 1 slave
    unsigned    log_len;             // log entries with seq 1..log_len
    unsigned    log_page;            // entries per LOG reply
    unsigned    runs[256];           // times each op was executed (replays not counted)
    unsigned    replays;             // replies served from the request-id store
    uint8_t     seen_args[256][12];  // the arguments (CDB 4-15) each op last saw
    int         force_result;        // >= 0: the next op answers with this result ...
    const char *force_message;       // ... and this message (NULL: none)
    bool        force_no_payload;    // ... and no JSON at all
} fakefw;

void fakefw_init(fakefw *fw, bool ext);
// Power cycle: new boot id, request-id store and state gone.
void fakefw_reboot(fakefw *fw);
// api_dispatch(): reply length in out (cap bytes), or -1 with sense set
// (CHECK CONDITION).
int  fakefw_dispatch(fakefw *fw, const uint8_t cdb[16], uint32_t cap, uint8_t *out, uint8_t sense[3]);

#endif
