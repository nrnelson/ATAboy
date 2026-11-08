#include "apiframe.h"
#include "crc32.h"
#include <string.h>

enum { S_MAGIC, S_TYPE, S_HDR, S_BODY, S_CRC };

void af_rx_reset(af_rx_t *rx) { memset(rx, 0, sizeof(*rx)); rx->state = S_MAGIC; }

bool af_rx_idle(const af_rx_t *rx) { return rx->state == S_MAGIC; }

size_t af_rx_feed(af_rx_t *rx, const uint8_t *p, size_t n, af_event_t *ev) {
    ev->kind = AF_EV_NONE;
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i++];
        switch (rx->state) {
        case S_MAGIC:
            if (b == AF_MAGIC) rx->state = S_TYPE;
            break;
        case S_TYPE:
            if (b == AF_REQ) {
                rx->hdr[0] = AF_MAGIC; rx->hdr[1] = AF_REQ;
                rx->pos = 2;
                rx->state = S_HDR;
            } else if (b != AF_MAGIC) {
                rx->state = S_MAGIC;
            }
            break;
        case S_HDR:
            rx->hdr[rx->pos++] = b;
            if (rx->pos < AF_HDR_LEN) break;
            rx->body_len = ((uint32_t)rx->hdr[8] << 8) | rx->hdr[9];
            if (rx->hdr[2] != AF_VERSION || rx->body_len < AF_CDB_LEN ||
                rx->body_len > AF_CDB_LEN + AF_MAX_BODY) {
                af_rx_reset(rx);                  // not a frame we know: hunt again
                break;
            }
            rx->crc = crc32_update(0, rx->hdr, AF_HDR_LEN);
            rx->pos = 0;
            rx->state = S_BODY;
            break;
        case S_BODY:
            if (rx->pos < AF_CDB_LEN) rx->cdb[rx->pos] = b;   // data-out is checked, not kept
            rx->crc = crc32_update(rx->crc, &b, 1);
            if (++rx->pos == rx->body_len) { rx->pos = 0; rx->state = S_CRC; }
            break;
        case S_CRC:
            rx->crcb[rx->pos++] = b;
            if (rx->pos < AF_CRC_LEN) break;
            {
                uint32_t want = ((uint32_t)rx->crcb[0] << 24) | ((uint32_t)rx->crcb[1] << 16) |
                                ((uint32_t)rx->crcb[2] << 8) | rx->crcb[3];
                ev->tag = (uint16_t)((rx->hdr[4] << 8) | rx->hdr[5]);
                ev->alloc = (uint16_t)((rx->hdr[6] << 8) | rx->hdr[7]);
                memcpy(ev->cdb, rx->cdb, AF_CDB_LEN);
                if (want != rx->crc) ev->kind = AF_EV_BAD;
                else if (rx->body_len > AF_CDB_LEN) ev->kind = AF_EV_UNSUPPORTED;
                else ev->kind = AF_EV_REQUEST;
            }
            af_rx_reset(rx);
            return i;
        }
    }
    return i;
}

uint32_t af_response(uint8_t *frame, uint16_t tag, uint8_t status, uint32_t body_len) {
    frame[0] = AF_MAGIC;
    frame[1] = AF_RSP;
    frame[2] = AF_VERSION;
    frame[3] = status;
    frame[4] = (uint8_t)(tag >> 8); frame[5] = (uint8_t)tag;
    frame[6] = 0; frame[7] = 0;
    frame[8] = (uint8_t)(body_len >> 8); frame[9] = (uint8_t)body_len;
    uint32_t crc = crc32_update(0, frame, AF_HDR_LEN + body_len);
    uint8_t *c = frame + AF_HDR_LEN + body_len;
    c[0] = (uint8_t)(crc >> 24); c[1] = (uint8_t)(crc >> 16); c[2] = (uint8_t)(crc >> 8); c[3] = (uint8_t)crc;
    return AF_HDR_LEN + body_len + AF_CRC_LEN;
}
