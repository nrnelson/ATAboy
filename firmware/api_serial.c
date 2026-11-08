// The ATAboy API on the "ATAboy API" USB serial port.
//
// Same requests, same replies and the same code path as the SCSI vendor
// command, so it needs no root (Linux) or admin (Windows) rights: only access
// to the serial device.  Runs from the core 0 main loop, outside tud_task(),
// so a request never overlaps a USB mass-storage command or an SCSI API
// request; the IDE bus itself is shared with the menu through ide_lock() as
// before.  One request at a time: the next is read only after the previous
// response has been handed to TinyUSB.

#include "api_serial.h"
#include "apiframe.h"
#include "api.h"
#include "usbbuf.h"
#include "tusb.h"
#include "pico/time.h"
#include <string.h>

#define API_CDC        1        // TinyUSB CDC instance (interface 3)
#define RX_STALL_MS    1000     // a frame that stops arriving for this long is dropped
#define MSC_QUIET_MS   500      // a mass-storage transfer counts as finished after this much quiet

static af_rx_t    rx;
static uint8_t    in[64];                 // bytes read from the CDC FIFO, not yet parsed
static uint32_t   in_len, in_off;
static uint32_t   last_rx_ms;
static af_event_t pending;                // a request waiting for the disk to go quiet
static bool       have_pending;
static uint8_t    frame[AF_HDR_LEN + AF_MAX_BODY + AF_CRC_LEN] __attribute__((aligned(4)));
static uint32_t   tx_len, tx_pos;
static uint32_t   msc_seen_seq, msc_quiet_since;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

bool api_serial_idle(void) { return tx_pos >= tx_len; }

// Port opened or closed (DTR): forget any half-received request and any
// response nobody will read.  TinyUSB clears its TX FIFO on DTR drop itself.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)dtr; (void)rts;
    if (itf != API_CDC) return;
    af_rx_reset(&rx);
    in_len = in_off = 0;
    tx_len = tx_pos = 0;
    tud_cdc_n_write_clear(API_CDC);
}

static void respond(uint16_t tag, uint8_t status, uint32_t body_len) {
    tx_len = af_response(frame, tag, status, body_len);
    tx_pos = 0;
}

// A READ(10)/WRITE(10) runs as several callbacks with tud_task() returning in
// between; a serial request must not land in such a gap (an UNMOUNT would fail
// the rest of the transfer, which the SCSI path can never do).  A transfer is
// over at its completion callback, or after MSC_QUIET_MS without a callback
// (a host that reset the device mid-command never completes it).
static bool msc_busy(uint32_t now) {
    uint32_t seq;
    if (!usbbuf_msc_active(&seq)) return false;
    if (seq != msc_seen_seq) { msc_seen_seq = seq; msc_quiet_since = now; }
    return now - msc_quiet_since < MSC_QUIET_MS;
}

static void execute(const af_event_t *ev) {
    uint32_t cap = ev->alloc ? ev->alloc : AF_MAX_BODY;
    if (cap > AF_MAX_BODY) cap = AF_MAX_BODY;
    uint8_t sense[3];
    int32_t n = api_dispatch(ev->cdb, frame + AF_HDR_LEN, cap, sense);
    if (n < 0) {
        memcpy(frame + AF_HDR_LEN, sense, 3);
        respond(ev->tag, AF_ST_CHECK, 3);
    } else {
        respond(ev->tag, AF_ST_OK, (uint32_t)n);
    }
}

void api_serial_task(void) {
    uint32_t now = now_ms();

    if (tx_pos < tx_len) {
        uint32_t room = tud_cdc_n_write_available(API_CDC);
        if (room) {
            uint32_t n = tx_len - tx_pos;
            if (n > room) n = room;
            tx_pos += tud_cdc_n_write(API_CDC, frame + tx_pos, n);
        }
        tud_cdc_n_write_flush(API_CDC);
        if (tx_pos < tx_len) return;
    }

    if (have_pending) {
        if (msc_busy(now)) return;
        have_pending = false;
        execute(&pending);
        return;
    }

    if (!af_rx_idle(&rx) && now - last_rx_ms > RX_STALL_MS) af_rx_reset(&rx);

    while (true) {
        if (in_off >= in_len) {
            if (!tud_cdc_n_available(API_CDC)) return;
            in_len = tud_cdc_n_read(API_CDC, in, sizeof(in));
            in_off = 0;
            last_rx_ms = now;
            if (!in_len) return;
        }
        af_event_t ev;
        in_off += (uint32_t)af_rx_feed(&rx, in + in_off, in_len - in_off, &ev);
        switch (ev.kind) {
        case AF_EV_REQUEST:
            pending = ev;
            have_pending = true;
            return;                       // runs on the next pass, once the disk is quiet
        case AF_EV_BAD:
            respond(ev.tag, AF_ST_BAD_FRAME, 0);
            return;
        case AF_EV_UNSUPPORTED:
            respond(ev.tag, AF_ST_UNSUPPORTED, 0);
            return;
        default:
            break;
        }
    }
}
