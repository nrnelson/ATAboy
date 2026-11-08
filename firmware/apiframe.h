#ifndef APIFRAME_H
#define APIFRAME_H

// Framing of the ATAboy API on the "ATAboy API" USB serial port
// (docs/API.md "API serial port").  Pure C (no SDK), so the host tests
// build it too.
//
// Request:  A5 'Q' ver flags tag[2] alloc[2] len[2] | body[len] | crc32[4]
//           body = the 16 CDB bytes (C0 op 'A' 'B'|'X' args) + data-out
// Response: A5 'R' ver status tag[2] 00 00 len[2]   | body[len] | crc32[4]
//           status 0: body = the API reply, byte for byte what the SCSI
//           path returns as data-in.
// Multi-byte fields are big-endian; the CRC is zlib's CRC-32 over every
// byte before it.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AF_MAGIC        0xA5
#define AF_REQ          'Q'
#define AF_RSP          'R'
#define AF_VERSION      1
#define AF_HDR_LEN      10
#define AF_CRC_LEN      4
#define AF_CDB_LEN      16
#define AF_MAX_BODY     4096          // reply bytes / request data-out bytes

// Response status
#define AF_ST_OK          0   // body = API reply
#define AF_ST_CHECK       1   // body = sense key, ASC, ASCQ (SCSI would say CHECK CONDITION)
#define AF_ST_BAD_FRAME   2   // request CRC mismatch: nothing was executed
#define AF_ST_UNSUPPORTED 3   // request carried data-out, which no op takes yet: not executed

// Parser events
#define AF_EV_NONE        0
#define AF_EV_REQUEST     1   // a good request: tag, alloc, cdb are set
#define AF_EV_BAD         2   // CRC mismatch (tag as received)
#define AF_EV_UNSUPPORTED 3   // good CRC but data-out present

typedef struct {
    uint8_t  kind;
    uint16_t tag;
    uint16_t alloc;           // as sent; 0 means AF_MAX_BODY
    uint8_t  cdb[AF_CDB_LEN];
} af_event_t;

typedef struct {
    uint8_t  state;
    uint32_t pos;             // bytes of the current state's field received
    uint32_t body_len;
    uint32_t crc;             // running CRC of header + body
    uint8_t  hdr[AF_HDR_LEN];
    uint8_t  cdb[AF_CDB_LEN];
    uint8_t  crcb[AF_CRC_LEN];
} af_rx_t;

void     af_rx_reset(af_rx_t *rx);
// Consume bytes until a frame completes (ev->kind != AF_EV_NONE) or the input
// runs out.  Returns the number of bytes consumed.
size_t   af_rx_feed(af_rx_t *rx, const uint8_t *p, size_t n, af_event_t *ev);
// True while no frame is partly received.
bool     af_rx_idle(const af_rx_t *rx);
// Finish a response whose body (body_len bytes) is already at frame +
// AF_HDR_LEN: writes the header and the CRC.  Returns the frame length.
uint32_t af_response(uint8_t *frame, uint16_t tag, uint8_t status, uint32_t body_len);

#endif
