// Fuzz target: the serial-port framing shared with the firmware
// (firmware/apiframe.c): the request parser the firmware runs on host bytes,
// and the response scanner the host runs on device bytes.
#include "apiframe.h"
#include "fuzz_main.h"
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Firmware side: feed in uneven pieces, as USB packets would arrive.
    af_rx_t rx;
    af_rx_reset(&rx);
    size_t i = 0, piece = size ? (size_t)(data[0] % 67) + 1 : 1;
    while (i < size) {
        size_t n = size - i < piece ? size - i : piece;
        while (n) {
            af_event_t ev;
            size_t used = af_rx_feed(&rx, data + i, n, &ev);
            if (used == 0 || used > n) __builtin_trap();
            i += used;
            n -= used;
            if (ev.kind == AF_EV_REQUEST) {          // answer it, as the firmware would
                static uint8_t frame[AF_HDR_LEN + AF_MAX_BODY + AF_CRC_LEN];
                uint32_t len = ev.alloc && ev.alloc < AF_MAX_BODY ? ev.alloc : AF_MAX_BODY;
                memset(frame + AF_HDR_LEN, 0x5A, len);
                if (af_response(frame, ev.tag, AF_ST_OK, len) != AF_HDR_LEN + len + AF_CRC_LEN) __builtin_trap();
            }
        }
    }
    (void)af_rx_idle(&rx);

    // Host side: scan a receive buffer the way transport_serial.c does.
    uint8_t buf[8192];
    size_t len = size < sizeof(buf) ? size : sizeof(buf);
    memcpy(buf, data, len);
    while (len) {
        af_frame_t f;
        bool found;
        size_t d = af_scan_response(buf, len, &f, &found);
        if (d > len) __builtin_trap();
        if (found) {
            if (f.body < buf || f.body + f.len > buf + len) __builtin_trap();
            volatile uint8_t sink = 0;
            for (size_t k = 0; k < f.len; k++) sink ^= f.body[k];
            (void)sink;
        }
        if (d == 0) break;                           // incomplete frame: needs more bytes
        memmove(buf, buf + d, len - d);
        len -= d;
    }
    return 0;
}
