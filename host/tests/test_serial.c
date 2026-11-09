// Serial transport tests (transport_serial.c + device.c) without hardware.
//
// The seam is the platform layer: this program is linked from device.c,
// transport_serial.c and the firmware's framing code, but not from
// plat_*.c / discover_*.c.  It supplies those functions itself: a fake clock
// and a fake serial port whose far end runs the firmware's own frame parser
// (af_rx_feed / af_response, as api_serial.c does) in front of fakefw.c.
// Faults (junk, stale frames, corrupted or lost replies, corrupted requests)
// are injected on the wire.
#include "internal.h"
#include "api_proto.h"
#include "apiframe.h"
#include "fakefw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---------------------------------------------------------------------------
//  Simulated wire and firmware
// ---------------------------------------------------------------------------

#define FRAME_MAX (AF_HDR_LEN + AF_MAX_BODY + AF_CRC_LEN)

static struct {
    fakefw   fw;
    unsigned write_fail_n;     // ab_serial_write fails (nothing sent)
    unsigned corrupt_req_n;    // a request byte flips on the way: firmware answers BAD_FRAME
    unsigned refuse_n;         // firmware answers UNSUPPORTED without running it
    unsigned corrupt_rsp_n;    // a reply byte flips on the way: CRC mismatch
    unsigned drop_rsp_n;       // the reply never arrives
    const char *junk;          // bytes before every reply
    bool     stale;            // a whole frame with an older tag before every reply
    size_t   chunk;            // bytes per read (reassembly)
    unsigned requests;         // frames the firmware parsed
    uint64_t now;
} sim;

struct ab_serial {
    af_rx_t rx;
    uint8_t out[4 * FRAME_MAX];
    size_t  out_len, out_pos;
};

static void queue(ab_serial *s, const uint8_t *p, size_t n) {
    if (s->out_len + n > sizeof(s->out)) abort();
    memcpy(s->out + s->out_len, p, n);
    s->out_len += n;
}

static void respond(ab_serial *s, uint16_t tag, uint8_t status, const uint8_t *body, uint32_t len) {
    static uint8_t frame[FRAME_MAX];
    if (len) memcpy(frame + AF_HDR_LEN, body, len);
    uint32_t n = af_response(frame, tag, status, len);
    if (sim.drop_rsp_n) { sim.drop_rsp_n--; return; }
    if (sim.junk) queue(s, (const uint8_t *)sim.junk, strlen(sim.junk));
    if (sim.stale) {
        static uint8_t old[FRAME_MAX];
        memcpy(old + AF_HDR_LEN, body, len);
        queue(s, old, af_response(old, (uint16_t)(tag - 1), status, len));
    }
    if (sim.corrupt_rsp_n) { sim.corrupt_rsp_n--; frame[AF_HDR_LEN] ^= 0x40; }
    queue(s, frame, n);
}

// The firmware side of one received frame (api_serial.c).
static void firmware_event(ab_serial *s, const af_event_t *ev) {
    static uint8_t body[API_MAX_REPLY];
    sim.requests++;
    if (ev->kind == AF_EV_BAD) { respond(s, ev->tag, AF_ST_BAD_FRAME, NULL, 0); return; }
    if (ev->kind != AF_EV_REQUEST || sim.refuse_n) {
        if (sim.refuse_n) sim.refuse_n--;
        respond(s, ev->tag, AF_ST_UNSUPPORTED, NULL, 0);
        return;
    }
    uint8_t sense[3];
    int n = fakefw_dispatch(&sim.fw, ev->cdb, ev->alloc ? ev->alloc : AF_MAX_BODY, body, sense);
    if (n < 0) respond(s, ev->tag, AF_ST_CHECK, sense, 3);
    else respond(s, ev->tag, AF_ST_OK, body, (uint32_t)n);
}

// ---------------------------------------------------------------------------
//  Platform layer stand-ins
// ---------------------------------------------------------------------------

uint64_t ab_now_ms(void) { return sim.now; }
void ab_sleep_ms(uint32_t ms) { sim.now += ms; }
uint32_t ab_random32(void) {
    static uint32_t x = 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
bool ab_is_serial_path(const char *path) { return path && strncmp(path, "fake-tty", 8) == 0; }

int ab_serial_open(const char *path, ab_serial **out) {
    if (strcmp(path, "fake-tty-denied") == 0) return ab_fail(AB_ERR_PERM, "no access to %s", path);
    ab_serial *s = calloc(1, sizeof(*s));
    if (!s) return ab_fail(AB_ERR_NOMEM, "out of memory");
    af_rx_reset(&s->rx);
    *out = s;
    return AB_OK;
}

int ab_serial_write(ab_serial *s, const uint8_t *buf, size_t n, uint64_t deadline_ms) {
    (void)deadline_ms;
    if (sim.write_fail_n) { sim.write_fail_n--; return ab_fail(AB_ERR_TRANSPORT, "fake: write failed"); }
    uint8_t tmp[64];
    if (n > sizeof(tmp)) abort();
    memcpy(tmp, buf, n);
    if (sim.corrupt_req_n) { sim.corrupt_req_n--; tmp[AF_HDR_LEN + 1] ^= 0x01; }
    size_t off = 0;
    while (off < n) {
        af_event_t ev;
        off += af_rx_feed(&s->rx, tmp + off, n - off, &ev);
        if (ev.kind != AF_EV_NONE) firmware_event(s, &ev);
    }
    return AB_OK;
}

long ab_serial_read(ab_serial *s, uint8_t *buf, size_t cap, uint64_t deadline_ms) {
    size_t left = s->out_len - s->out_pos;
    if (!left) {                        // nothing more is coming: the deadline passes
        if (sim.now < deadline_ms) sim.now = deadline_ms;
        return 0;
    }
    size_t k = left < cap ? left : cap;
    if (sim.chunk && k > sim.chunk) k = sim.chunk;
    memcpy(buf, s->out + s->out_pos, k);
    s->out_pos += k;
    if (s->out_pos == s->out_len) s->out_pos = s->out_len = 0;
    return (long)k;
}

void ab_serial_drain(ab_serial *s) { s->out_pos = s->out_len = 0; }
void ab_serial_close(ab_serial *s) { free(s); }

int ab_find_devices(ab_devinfo *out, int max) { (void)out; (void)max; return 0; }
void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms) {
    (void)d; (void)sectors; (void)want_ro; (void)wait_ms;
}
int ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s) { (void)d; (void)need_s; return -1; }
#if defined(__linux__)
int ab_sgio_transport_open(const char *path, ab_transport **out) {
    (void)out;
    return ab_fail(AB_ERR_NODEV, "fake: no SG_IO for %s", path);
}
#endif

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

static ab_dev *open_fake(bool ext) {
    uint64_t now = sim.now;
    memset(&sim, 0, sizeof(sim));
    sim.now = now;
    sim.chunk = 7;
    fakefw_init(&sim.fw, ext);
    ab_dev *d = NULL;
    int rc = ab_open(&d, "fake-tty0", NULL, "serial");
    CHECK(rc == AB_OK && d);
    if (!d) exit(1);
    return d;
}

static void test_open(void) {
    ab_dev *d = NULL;
    CHECK(ab_open(&d, "fake-tty0", NULL, "auto") == AB_OK);
    CHECK(!strcmp(ab_transport_name(d), "serial") && ab_dev_info(d) == NULL);
    CHECK(!strcmp(ab_dev_key(d), "fake-tty0"));
    ab_close(d);
    CHECK(ab_open(&d, "fake-tty-denied", NULL, "serial") == AB_ERR_PERM && d == NULL);
    CHECK(ab_open(&d, "fake-tty0", NULL, "sgio") == AB_ERR_ARG && d == NULL);
    CHECK(ab_open(&d, "fake-tty0", NULL, "usb") == AB_ERR_ARG && d == NULL);
    CHECK(ab_open(&d, NULL, NULL, NULL) == AB_ERR_NODEV && d == NULL);
}

static void test_status_and_info(void) {
    ab_dev *d = open_fake(true);
    cJSON *j = NULL;
    CHECK(ab_status(d, &j) == AB_OK);
    CHECK(cJSON_IsBool(ab_jobj_get(j, "mounted")));
    cJSON_Delete(j);
    sim.fw.log_len = 40;                // replies of a few hundred bytes, in 7-byte reads
    sim.fw.log_page = 40;
    CHECK(ab_log(d, &j) == AB_OK);
    CHECK(cJSON_GetArraySize(ab_jobj_get(j, "entries")) == 40);
    cJSON_Delete(j);
    uint32_t b;
    CHECK(ab_boot_id(d, &b) && b == sim.fw.boot_id);
    CHECK(sim.requests == 2);
    ab_close(d);
}

static void test_check_condition_relayed(void) {
    ab_dev *d = open_fake(true);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    ab_reply_free(&r);
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 100, 0, &r) == AB_ERR_CHECK);
    uint8_t k, asc, q;
    ab_sense(d, &k, &asc, &q);
    CHECK(k == 5 && asc == 0x24 && q == 0);
    ab_close(d);

    d = open_fake(false);               // older firmware: refuses 'X', basic from then on
    CHECK(ab_mount(d, false, NULL) == AB_OK);
    CHECK(sim.fw.runs[OP_MOUNT] == 1 && sim.requests == 2);
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK && !r.ext);
    ab_reply_free(&r);
    CHECK(sim.requests == 3);
    ab_close(d);
}

static void test_junk_and_stale_frames_skipped(void) {
    ab_dev *d = open_fake(true);
    sim.junk = "AT\r\n\xa5\xa5R\x07";
    sim.stale = true;
    for (int i = 0; i < 3; i++) {
        ab_reply r;
        CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK && r.result == R_OK);
        ab_reply_free(&r);
    }
    CHECK(sim.fw.runs[OP_STATUS] == 3 && d->empty_replies == 0);
    ab_close(d);
}

static void test_corrupt_reply_resends_read_only(void) {
    ab_dev *d = open_fake(true);
    sim.corrupt_rsp_n = 1;
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(d->empty_replies == 1 && sim.fw.runs[OP_STATUS] == 2);
    ab_reply_free(&r);
    ab_close(d);
}

static void test_corrupt_reply_state_change_runs_once(void) {
    ab_dev *d = open_fake(true);
    sim.corrupt_rsp_n = 1;
    uint8_t a[1] = {0};
    ab_reply r;
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_OK);
    CHECK(r.result == R_OK && r.replayed && r.rid_taken);
    CHECK(sim.fw.runs[OP_MOUNT] == 1 && sim.fw.replays == 1);
    ab_reply_free(&r);
    ab_close(d);

    d = open_fake(true);                // basic header: no id, so not again
    d->ext = 0;
    sim.corrupt_rsp_n = 1;
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(strstr(ab_errmsg(d), "CRC") != NULL);
    CHECK(sim.fw.runs[OP_MOUNT] == 1 && sim.requests == 1);
    ab_close(d);
}

static void test_lost_reply_state_change_runs_once(void) {
    ab_dev *d = open_fake(true);
    sim.drop_rsp_n = 1;
    ab_reply r;
    uint64_t t0 = sim.now;
    CHECK(ab_call(d, OP_UNMOUNT, NULL, 0, 0, 200, &r) == AB_OK);
    CHECK(r.replayed && sim.fw.runs[OP_UNMOUNT] == 1);
    CHECK(sim.now - t0 == 200 + 2000);  // waited out one timeout (+ slack), not more
    ab_reply_free(&r);
    ab_close(d);
}

static void test_request_corrupted_on_the_way_is_resent(void) {
    ab_dev *d = open_fake(true);
    sim.corrupt_req_n = 1;
    uint8_t a[1] = {0};
    ab_reply r;
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_OK);
    CHECK(sim.fw.runs[OP_MOUNT] == 1 && sim.fw.replays == 0 && sim.requests == 2);
    ab_reply_free(&r);
    ab_close(d);

    d = open_fake(true);                // refused frame, write failure: also not executed
    d->ext = 0;
    sim.refuse_n = 1;
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_OK);
    CHECK(sim.fw.runs[OP_MOUNT] == 1 && sim.requests == 2);
    ab_reply_free(&r);
    sim.write_fail_n = 1;
    CHECK(ab_call(d, OP_UNMOUNT, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(sim.fw.runs[OP_UNMOUNT] == 1);
    ab_reply_free(&r);
    sim.write_fail_n = 2;
    CHECK(ab_call(d, OP_UNMOUNT, NULL, 0, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(sim.fw.runs[OP_UNMOUNT] == 1);
    ab_close(d);
}

static void test_two_losses_give_up(void) {
    ab_dev *d = open_fake(true);
    sim.drop_rsp_n = 2;
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 100, &r) == AB_ERR_TRANSPORT);
    CHECK(strstr(ab_errmsg(d), "no reply") != NULL);
    CHECK(sim.fw.runs[OP_STATUS] == 2);
    ab_close(d);
}

int main(void) {
    test_open();
    test_status_and_info();
    test_check_condition_relayed();
    test_junk_and_stale_frames_skipped();
    test_corrupt_reply_resends_read_only();
    test_corrupt_reply_state_change_runs_once();
    test_lost_reply_state_change_runs_once();
    test_request_corrupted_on_the_way_is_resent();
    test_two_losses_give_up();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all serial transport tests passed\n");
    return 0;
}
