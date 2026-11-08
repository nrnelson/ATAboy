// API requests over the ATAboy's "ATAboy API" USB serial port.  Framing:
// firmware/apiframe.[ch] (the same code the firmware runs).

#include "internal.h"
#include "apiframe.h"
#include <stdlib.h>
#include <string.h>

#define SLACK_MS 2000                     // on top of the request's own timeout
#define RXBUF    (2 * (AF_HDR_LEN + AF_MAX_BODY + AF_CRC_LEN))

typedef struct {
    ab_transport base;
    ab_serial   *port;
    uint16_t     tag;
    uint8_t      buf[RXBUF];
    size_t       len;
    unsigned     stale_frames;           // answers to older requests, discarded
} serial_tp;

static void drop(serial_tp *t, size_t n) {
    if (n >= t->len) { t->len = 0; return; }
    memmove(t->buf, t->buf + n, t->len - n);
    t->len -= n;
}

static int sp_api(ab_transport *base, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms,
                  uint8_t *out, size_t *out_len, bool *maybe_executed, uint8_t sense[3]) {
    serial_tp *t = (serial_tp *)base;
    *maybe_executed = false;
    *out_len = 0;
    t->tag++;
    uint64_t deadline = ab_now_ms() + timeout_ms + SLACK_MS;
    ab_serial_drain(t->port);               // leftovers of an abandoned request
    t->len = 0;
    uint8_t frame[AF_HDR_LEN + AF_CDB_LEN + AF_CRC_LEN];
    uint32_t n = af_request(frame, t->tag, alloc, cdb);
    int rc = ab_serial_write(t->port, frame, n, deadline);
    if (rc != AB_OK) return rc;               // not (fully) sent: not executed
    *maybe_executed = true;
    for (;;) {
        af_frame_t f;
        bool found;
        size_t d = af_scan_response(t->buf, t->len, &f, &found);
        if (found) {
            if (!f.crc_ok) {
                drop(t, d);
                return ab_fail(AB_ERR_TRANSPORT, "reply failed its CRC check");
            }
            if (f.tag != t->tag) {            // an answer to an older request
                t->stale_frames++;
                drop(t, d);
                continue;
            }
            uint8_t status = f.status;
            size_t len = f.len;
            uint8_t body3[3] = {0, 0, 0};
            if (status == AF_ST_OK) {
                if (len > alloc) len = alloc;
                memcpy(out, f.body, len);
                *out_len = len;
            } else if (len >= 3) {
                memcpy(body3, f.body, 3);
            }
            drop(t, d);
            switch (status) {
            case AF_ST_OK:
                return AB_OK;
            case AF_ST_CHECK:
                memcpy(sense, body3, 3);
                return ab_fail(AB_ERR_CHECK, "CHECK CONDITION sense %X/%02X/%02X", body3[0], body3[1], body3[2]);
            case AF_ST_BAD_FRAME:
                *maybe_executed = false;
                return ab_fail(AB_ERR_TRANSPORT, "the ATAboy received a corrupted request (not executed)");
            default:
                *maybe_executed = false;
                return ab_fail(AB_ERR_TRANSPORT, "the ATAboy refused the request frame (status %u)", status);
            }
        }
        drop(t, d);
        if (t->len == sizeof(t->buf)) t->len = 0;   // can't happen with a sane scanner; never spin
        long k = ab_serial_read(t->port, t->buf + t->len, sizeof(t->buf) - t->len, deadline);
        if (k < 0) return (int)k;
        if (k == 0) return ab_fail(AB_ERR_TRANSPORT, "no reply from the ATAboy within the timeout");
        t->len += (size_t)k;
    }
}

static void sp_close(ab_transport *base) {
    serial_tp *t = (serial_tp *)base;
    ab_serial_close(t->port);
    free(t);
}

int ab_serial_transport_open(const char *path, ab_transport **out) {
    serial_tp *t = calloc(1, sizeof(*t));
    if (!t) return ab_fail(AB_ERR_NOMEM, "out of memory");
    int rc = ab_serial_open(path, &t->port);
    if (rc != AB_OK) { free(t); return rc; }
    t->base.name = "serial";
    t->base.api = sp_api;
    t->base.close = sp_close;
    t->tag = (uint16_t)ab_random32();
    *out = &t->base;
    return AB_OK;
}
