// Request engine tests (device.c): a fake transport in front of a model of the
// firmware's api_dispatch() (fakefw.c), so extended-header detection, request
// ids, resends, boot ids and reply parsing are checked against what the
// firmware really does.  Ported from tests/test_host_lib.py where they apply.
#include "internal.h"
#include "api_proto.h"
#include "fakefw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// ---------------------------------------------------------------------------
//  Fake transport
// ---------------------------------------------------------------------------

#define MAX_CDBS 512

typedef struct {
    ab_transport   base;
    fakefw        *fw;
    int            fault_op;          // faults apply to this op only (-1: any)
    int            empty_n;           // run it, GOOD status but no data
    int            lost_n;            // run it, reply lost (maybe executed)
    int            unsent_n;          // request never reached the device
    const uint8_t *raw;               // answer these bytes instead (nothing runs)
    size_t         raw_len;
    uint8_t        cdbs[MAX_CDBS][16];
    uint32_t       allocs[MAX_CDBS];
    unsigned       ncdb;
} fake_tp;

static int ft_api(ab_transport *base, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms,
                  uint8_t *out, size_t *out_len, bool *maybe_executed, uint8_t sense[3]) {
    fake_tp *t = (fake_tp *)base;
    *out_len = 0;
    *maybe_executed = false;
    if (t->ncdb < MAX_CDBS) {
        memcpy(t->cdbs[t->ncdb], cdb, 16);
        t->allocs[t->ncdb] = alloc;
        t->ncdb++;
    }
    bool hit = t->fault_op < 0 || t->fault_op == cdb[1];
    if (hit && t->unsent_n) {
        t->unsent_n--;
        return ab_fail(AB_ERR_TRANSPORT, "fake: write failed");
    }
    if (t->raw) {
        size_t n = t->raw_len < alloc ? t->raw_len : alloc;
        memcpy(out, t->raw, n);
        *out_len = n;
        return AB_OK;
    }
    static uint8_t buf[API_MAX_REPLY];
    int n = fakefw_dispatch(t->fw, cdb, alloc, buf, sense);
    if (n < 0) return ab_fail(AB_ERR_CHECK, "fake: CHECK CONDITION %X/%02X/%02X", sense[0], sense[1], sense[2]);
    if (hit && t->empty_n) { t->empty_n--; return AB_OK; }
    if (hit && t->lost_n) {
        t->lost_n--;
        *maybe_executed = true;
        return ab_fail(AB_ERR_TRANSPORT, "fake: reply lost");
    }
    if ((uint32_t)n > alloc) n = (int)alloc;
    memcpy(out, buf, (size_t)n);
    *out_len = (size_t)n;
    return AB_OK;
}

static void ft_close(ab_transport *base) { (void)base; }

static fakefw  fw;
static fake_tp tp;

// A device on the fake transport; ext = what the firmware supports.
static ab_dev *mkdev(bool ext) {
    fakefw_init(&fw, ext);
    memset(&tp, 0, sizeof(tp));
    tp.base.name = "fake";
    tp.base.api = ft_api;
    tp.base.close = ft_close;
    tp.fw = &fw;
    tp.fault_op = -1;
    ab_dev *d = calloc(1, sizeof(*d));
    if (!d) abort();
    d->ext = -1;
    d->tp = &tp.base;
    snprintf(d->path, sizeof(d->path), "fake");
    return d;
}

static uint32_t cdb_rid(unsigned i) { return ab_be32(&tp.cdbs[i][12]); }

// ---------------------------------------------------------------------------
//  Protocol tables
// ---------------------------------------------------------------------------

static void test_op_tables(void) {
    // The firmware's INFO rid_ops and the Python client's lists.
    static const uint8_t rid_ops[] = {0x03, 0x04, 0x05, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x25, 0x31, 0x40, 0x41};
    static const uint8_t ro_ops[] = {0x00, 0x01, 0x02, 0x20, 0x21, 0x22, 0x23, 0x24, 0x26, 0x30};
    for (int op = 0; op < 256; op++) {
        bool r = memchr(rid_ops, op, sizeof(rid_ops)) != NULL, ro = memchr(ro_ops, op, sizeof(ro_ops)) != NULL;
        CHECK(api_op_takes_rid((uint8_t)op) == r);
        CHECK(api_op_read_only((uint8_t)op) == ro);
    }
}

// ---------------------------------------------------------------------------
//  Extended header detection
// ---------------------------------------------------------------------------

static void test_ext_detected(void) {
    ab_dev *d = mkdev(true);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(r.ext && d->ext == 1);
    CHECK(r.boot_id == fw.boot_id);
    CHECK(tp.cdbs[0][2] == 'A' && tp.cdbs[0][3] == 'X');
    CHECK(tp.allocs[0] == API_MAX_REPLY);
    CHECK(r.json && !strcmp(ab_jstr(r.json, "result", ""), "ok"));
    uint32_t b = 0;
    CHECK(ab_boot_id(d, &b) && b == fw.boot_id);
    ab_reply_free(&r);
    CHECK(r.json == NULL);
    ab_close(d);
}

static void test_old_firmware_falls_back_to_basic(void) {
    ab_dev *d = mkdev(false);
    cJSON *j = NULL;
    CHECK(ab_mount(d, false, &j) == AB_OK);
    CHECK(ab_jbool(j, "mounted", false));
    cJSON_Delete(j);
    CHECK(d->ext == 0);
    CHECK(tp.ncdb == 2);
    CHECK(tp.cdbs[0][3] == 'X' && cdb_rid(0) != 0);      // refused: 5/24/00, nothing ran
    CHECK(tp.cdbs[1][3] == 'B' && cdb_rid(1) == 0);      // basic, the id cleared
    CHECK(fw.runs[OP_MOUNT] == 1);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(!r.ext && tp.ncdb == 3 && tp.cdbs[2][3] == 'B');
    ab_reply_free(&r);
    uint32_t b;
    CHECK(!ab_boot_id(d, &b));
    cJSON *li = ab_link_info(d);
    CHECK(!ab_jbool(li, "extended", true));
    CHECK(cJSON_IsNull(ab_jobj_get(li, "boot_id")));
    CHECK(!strcmp(ab_jstr(li, "transport", ""), "fake"));
    cJSON_Delete(li);
    ab_close(d);
}

static void test_check_condition_once_extended(void) {
    // Once the extended header is known to work, a CHECK CONDITION is the
    // caller's problem (here: allocation below 256), not a reason to fall back.
    ab_dev *d = mkdev(true);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    ab_reply_free(&r);
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 100, 0, &r) == AB_ERR_CHECK);
    uint8_t k, asc, q;
    ab_sense(d, &k, &asc, &q);
    CHECK(k == 5 && asc == 0x24 && q == 0);
    CHECK(d->ext == 1 && tp.ncdb == 2);
    CHECK(strstr(ab_errmsg(d), "CHECK CONDITION") != NULL);
    ab_close(d);
}

// ---------------------------------------------------------------------------
//  Request ids
// ---------------------------------------------------------------------------

static void test_ids_only_on_state_changing_ops(void) {
    ab_dev *d = mkdev(true);
    cJSON *j = NULL;
    CHECK(ab_status(d, &j) == AB_OK);
    cJSON_Delete(j);
    CHECK(ab_config_set(d, "no_retry", "1", &j) == AB_OK);
    cJSON_Delete(j);
    CHECK(ab_config_set(d, "no_retry", "1", &j) == AB_OK);
    CHECK(ab_jbool(j, "no_retry", false));
    cJSON_Delete(j);
    CHECK(tp.ncdb == 3);
    for (unsigned i = 0; i < 3; i++) CHECK(tp.cdbs[i][2] == 'A' && tp.cdbs[i][3] == 'X');
    CHECK(cdb_rid(0) == 0);
    CHECK(cdb_rid(1) && cdb_rid(2) && cdb_rid(1) != cdb_rid(2));   // a fresh id per call
    static const uint8_t zero[4];
    CHECK(!memcmp(&fw.seen_args[OP_CONFIG_SET][8], zero, 4));       // the op never sees the id
    CHECK(fw.runs[OP_CONFIG_SET] == 2 && fw.replays == 0);
    ab_close(d);
}

static void test_rid_on_every_op(void) {
    for (int ext = 0; ext <= 1; ext++) {
        ab_dev *d = mkdev(ext != 0);
        d->ext = ext;
        for (int op = 0; op <= 0x41; op++) {
            ab_reply r;
            unsigned before = tp.ncdb;
            CHECK(ab_call(d, (uint8_t)op, NULL, 0, 0, 0, &r) == AB_OK);
            CHECK(tp.ncdb == before + 1);
            uint32_t rid = cdb_rid(before);
            bool want = ext && api_op_takes_rid((uint8_t)op);
            CHECK((rid != 0) == want);
            CHECK(tp.cdbs[before][3] == (ext ? 'X' : 'B'));
            CHECK(r.ext == (ext != 0));
            CHECK(r.rid_taken == want);
            if (want) CHECK(r.request_id == rid);
            ab_reply_free(&r);
        }
        ab_close(d);
    }
}

static void test_args_overlapping_rid_refused(void) {
    ab_dev *d = mkdev(true);
    d->ext = 1;
    uint8_t a[12] = {0};
    a[9] = 1;
    ab_reply r;
    CHECK(ab_call(d, OP_MOUNT, a, sizeof(a), 0, 0, &r) == AB_ERR_ARG);
    CHECK(tp.ncdb == 0);
    CHECK(ab_call(d, OP_STATUS, a, sizeof(a), 0, 0, &r) == AB_OK);    // no id: bytes 12-15 are free
    CHECK(tp.cdbs[0][13] == 1);
    ab_reply_free(&r);
    CHECK(ab_call(d, OP_STATUS, a, 13, 0, 0, &r) == AB_ERR_ARG);
    ab_close(d);
}

// ---------------------------------------------------------------------------
//  Resends
// ---------------------------------------------------------------------------

static void test_empty_reply_read_only_resent(void) {
    ab_dev *d = mkdev(false);
    tp.fault_op = OP_STATUS;
    tp.empty_n = 1;
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(r.result == R_OK && d->empty_replies == 1);
    CHECK(fw.runs[OP_STATUS] == 2);
    ab_reply_free(&r);
    ab_close(d);
}

static void test_empty_reply_state_change_not_resent(void) {
    ab_dev *d = mkdev(false);
    d->ext = 0;                                 // firmware without request ids
    tp.fault_op = OP_MOUNT;
    tp.empty_n = 1;
    cJSON *j = NULL;
    CHECK(ab_mount(d, false, &j) == AB_ERR_TRANSPORT);
    CHECK(j == NULL);
    CHECK(d->empty_replies == 0 && fw.runs[OP_MOUNT] == 1 && tp.ncdb == 1);
    CHECK(strstr(ab_errmsg(d), "empty reply") != NULL);
    ab_close(d);
}

static void test_empty_reply_state_change_resent_with_same_id(void) {
    ab_dev *d = mkdev(true);
    tp.fault_op = OP_MOUNT;
    tp.empty_n = 1;                             // it ran, but the reply got lost
    ab_reply r;
    uint8_t a[1] = {0};
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_OK);
    CHECK(r.result == R_OK && r.replayed && r.rid_taken);
    CHECK(tp.ncdb == 2 && !memcmp(tp.cdbs[0], tp.cdbs[1], 16));
    CHECK(fw.runs[OP_MOUNT] == 1 && fw.replays == 1 && d->empty_replies == 1);
    ab_reply_free(&r);
    ab_close(d);
}

static void test_other_ops_not_resent(void) {
    // Neither read-only nor taking an id (an op this client doesn't know):
    // never sent twice, even with the extended header.
    ab_dev *d = mkdev(true);
    d->ext = 1;
    tp.empty_n = 1;
    ab_reply r;
    CHECK(ab_call(d, 0x7F, NULL, 0, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(tp.ncdb == 1 && cdb_rid(0) == 0);
    tp.lost_n = 1;
    CHECK(ab_call(d, 0x7F, NULL, 0, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(tp.ncdb == 2 && fw.runs[0x7F] == 2);
    ab_close(d);
}

static void test_lost_reply(void) {
    // read-only: asked again
    ab_dev *d = mkdev(false);
    d->ext = 0;
    tp.lost_n = 1;
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(fw.runs[OP_STATUS] == 2 && tp.ncdb == 2);
    ab_reply_free(&r);
    ab_close(d);

    // state-changing without a request id: it may have run, so not again
    d = mkdev(false);
    d->ext = 0;
    tp.lost_n = 1;
    uint8_t a[1] = {0};
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(fw.runs[OP_MOUNT] == 1 && tp.ncdb == 1);
    CHECK(strstr(ab_errmsg(d), "lost") != NULL);
    ab_close(d);

    // with a request id: same id again, the firmware replays its reply
    d = mkdev(true);
    tp.lost_n = 1;
    CHECK(ab_call(d, OP_UNMOUNT, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(r.replayed && fw.runs[OP_UNMOUNT] == 1 && fw.replays == 1);
    CHECK(tp.ncdb == 2 && !memcmp(tp.cdbs[0], tp.cdbs[1], 16));
    ab_reply_free(&r);
    ab_close(d);
}

static void test_unsent_request_resent(void) {
    // The device never saw it: safe to send any op again.
    ab_dev *d = mkdev(false);
    d->ext = 0;
    tp.unsent_n = 1;
    uint8_t a[1] = {0};
    ab_reply r;
    CHECK(ab_call(d, OP_MOUNT, a, 1, 0, 0, &r) == AB_OK);
    CHECK(fw.runs[OP_MOUNT] == 1 && fw.replays == 0 && tp.ncdb == 2);
    ab_reply_free(&r);
    ab_close(d);
}

static void test_two_losses_give_up(void) {
    ab_dev *d = mkdev(true);
    tp.lost_n = 2;
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(tp.ncdb == 2);
    tp.lost_n = 0;
    tp.empty_n = 2;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_ERR_TRANSPORT);
    CHECK(tp.ncdb == 4);
    ab_close(d);
}

// ---------------------------------------------------------------------------
//  Boot ids
// ---------------------------------------------------------------------------

typedef struct { unsigned n; uint32_t old_id, new_id; uint8_t op; } reboot_seen;

static void on_reboot(void *ctx, uint32_t old_id, uint32_t new_id, uint8_t op) {
    reboot_seen *s = ctx;
    s->n++;
    s->old_id = old_id;
    s->new_id = new_id;
    s->op = op;
}

static void test_reboot_detected(void) {
    ab_dev *d = mkdev(true);
    reboot_seen seen = {0};
    ab_on_reboot(d, on_reboot, &seen);
    ab_reply r1, r2;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r1) == AB_OK);
    CHECK(!r1.rebooted);
    ab_reply_free(&r1);
    uint32_t old_id = fw.boot_id;
    fakefw_reboot(&fw);
    CHECK(ab_call(d, OP_INFO, NULL, 0, 0, 0, &r2) == AB_OK);
    CHECK(r2.rebooted && r2.boot_id == fw.boot_id);
    ab_reply_free(&r2);
    CHECK(seen.n == 1 && seen.old_id == old_id && seen.new_id == fw.boot_id && seen.op == OP_INFO);
    CHECK(d->reboots == 1);
    cJSON *li = ab_link_info(d);
    CHECK(ab_jnum(li, "reboots_seen", -1) == 1);
    CHECK(ab_jbool(li, "extended", false));
    CHECK(ab_jnum(li, "boot_id", -1) == fw.boot_id);
    const cJSON *lr = ab_jobj_get(li, "last_reboot");
    CHECK(ab_jnum(lr, "old_boot_id", -1) == old_id);
    CHECK(ab_jnum(lr, "new_boot_id", -1) == fw.boot_id);
    CHECK(ab_jnum(lr, "op", -1) == OP_INFO);
    CHECK(ab_jnum(lr, "uptime_ms", -1) == 510);
    cJSON_Delete(li);
    // no change: no callback
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r1) == AB_OK);
    CHECK(!r1.rebooted && seen.n == 1);
    ab_reply_free(&r1);
    ab_close(d);
}

static void test_reboot_since_seeded_id(void) {
    // A new instance seeded with the id a previous process saw notices a
    // reboot in between.
    ab_dev *d = mkdev(true);
    uint32_t seen_before = fw.boot_id;
    fakefw_reboot(&fw);
    ab_seed_boot_id(d, seen_before);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(r.rebooted && d->reboots == 1);
    ab_reply_free(&r);
    ab_close(d);

    d = mkdev(true);
    ab_seed_boot_id(d, fw.boot_id);
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(!r.rebooted && d->reboots == 0);
    ab_reply_free(&r);
    cJSON *li = ab_link_info(d);
    CHECK(cJSON_IsNull(ab_jobj_get(li, "last_reboot")));
    cJSON_Delete(li);
    ab_close(d);
}

static void test_state_counter(void) {
    ab_dev *d = mkdev(true);
    ab_reply r;
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    uint32_t a = r.state_counter;
    ab_reply_free(&r);
    CHECK(ab_mount(d, false, NULL) == AB_OK);
    CHECK(ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r) == AB_OK);
    uint32_t b = r.state_counter;
    ab_reply_free(&r);
    CHECK(b > a);
    cJSON *li = ab_link_info(d);
    CHECK(ab_jnum(li, "state_counter", -1) == b);
    cJSON_Delete(li);
    ab_close(d);
}

// ---------------------------------------------------------------------------
//  Reply parsing
// ---------------------------------------------------------------------------

static size_t mkreply(uint8_t *b, uint8_t proto, uint8_t op, uint8_t result, uint8_t ctype, const char *payload,
                      size_t plen_field, uint32_t uptime, uint8_t flags) {
    memset(b, 0, HDR_LEN);
    b[0] = 'A'; b[1] = 'B'; b[2] = proto; b[3] = op; b[4] = result; b[5] = ctype;
    ab_put_be16(&b[6], (uint16_t)plen_field);
    ab_put_be32(&b[8], uptime);
    b[12] = flags;
    size_t n = payload ? strlen(payload) : 0;
    if (n) memcpy(b + HDR_LEN, payload, n);
    return HDR_LEN + n;
}

static void test_reply_decoding(void) {
    ab_dev *d = mkdev(true);
    uint8_t raw[256];
    const char *p = "{\"result\":\"bad_arg\",\"message\":\"nope\"}";
    size_t n = mkreply(raw, 1, OP_READ, R_BAD_ARG, CT_JSON, p, strlen(p), 4242, 0x13);
    memset(raw + n, 0, 7);                       // trailing bytes past the payload are ignored
    tp.raw = raw;
    tp.raw_len = n + 7;
    ab_reply r;
    CHECK(ab_call(d, OP_READ, NULL, 0, 0, 0, &r) == AB_OK);
    CHECK(r.proto == 1 && r.op == OP_READ && r.result == R_BAD_ARG && r.ctype == CT_JSON);
    CHECK(r.uptime_ms == 4242 && r.flags == 0x13 && !r.ext);
    CHECK(r.payload_len == strlen(p) && r.payload == r.raw + HDR_LEN);
    CHECK(!strcmp(ab_jstr(r.json, "message", ""), "nope"));
    CHECK(!strcmp(ab_result_name(r.result), "bad_arg"));
    CHECK(ab_result_name(99) != NULL);
    ab_reply_free(&r);
    ab_close(d);
}

static void expect_bad_reply(const uint8_t *raw, size_t len, const char *why) {
    ab_dev *d = mkdev(true);
    tp.raw = raw;
    tp.raw_len = len;
    ab_reply r;
    int rc = ab_call(d, OP_STATUS, NULL, 0, 0, 0, &r);
    CHECK(rc == AB_ERR_TRANSPORT);
    CHECK(r.json == NULL);
    if (!strstr(ab_errmsg(d), why)) {
        printf("FAIL expected \"%s\" in \"%s\"\n", why, ab_errmsg(d));
        failures++;
    }
    cJSON *j = (cJSON *)1;
    CHECK(ab_status(d, &j) == AB_ERR_TRANSPORT && j == NULL);
    ab_close(d);
}

static void test_reply_errors(void) {
    uint8_t raw[256];
    expect_bad_reply((const uint8_t *)"AB\x01", 3, "short reply");
    mkreply(raw, 1, 0, 0, CT_NONE, NULL, 0, 0, 0);
    raw[0] = 'X'; raw[1] = 'Y';
    expect_bad_reply(raw, HDR_LEN, "bad reply magic");
    size_t n = mkreply(raw, 1, 0, 0, CT_BINARY, "xxxxxxxxxx", 100, 0, 0);
    expect_bad_reply(raw, n, "truncated reply");
    n = mkreply(raw, 1, 0, 0, CT_JSON, "{x}", 3, 0, 0);
    expect_bad_reply(raw, n, "malformed JSON");
    n = mkreply(raw, 1, 0, 0, CT_JSON, "{\"result\":", 10, 0, 0);
    expect_bad_reply(raw, n, "malformed JSON");
    n = mkreply(raw, 2, OP_STATUS, 0, CT_JSON, "{}", 2, 0, 0);
    expect_bad_reply(raw, n, "protocol 2");
    // extended header announced, but cut short
    mkreply(raw, 1, OP_STATUS, 0, CT_NONE, NULL, 0, 0, 0);
    raw[13] = EXT_HDR_LEN;
    expect_bad_reply(raw, 20, "short extended reply");
    // extended header with a payload running past the end
    memset(raw + HDR_LEN, 0, 16);
    ab_put_be16(&raw[6], 8);
    expect_bad_reply(raw, EXT_HDR_LEN + 4, "truncated reply");
    // nothing at all, for a read-only op: asked twice, then given up
    expect_bad_reply(raw, 0, "empty reply");
}

// ---------------------------------------------------------------------------
//  JSON operations
// ---------------------------------------------------------------------------

static void test_json_op_results(void) {
    ab_dev *d = mkdev(true);
    cJSON *j = NULL;
    CHECK(ab_info(d, &j) == AB_OK);
    CHECK(!strcmp(ab_jstr(j, "fw", ""), "fake-api1") && d->max_read_sectors == 7);
    cJSON_Delete(j);

    fw.force_result = R_NOT_MOUNTED;
    fw.force_message = "mount first";
    CHECK(ab_status(d, &j) == AB_ERR_API);
    CHECK(!strcmp(ab_jstr(j, "result", ""), "not_mounted"));
    CHECK(!strcmp(ab_jstr(j, "message", ""), "mount first"));
    CHECK(!strcmp(ab_errmsg(d), "not_mounted: mount first"));
    cJSON_Delete(j);

    fw.force_result = R_NOT_MOUNTED;            // without an out pointer: nothing leaks
    fw.force_message = NULL;
    CHECK(ab_status(d, NULL) == AB_ERR_API);
    CHECK(!strcmp(ab_errmsg(d), "not_mounted"));

    fw.force_result = R_NO_DRIVE;               // a result with no JSON
    fw.force_no_payload = true;
    CHECK(ab_unmount(d, &j) == AB_ERR_API);
    CHECK(!strcmp(ab_jstr(j, "result", ""), "no_drive"));
    cJSON_Delete(j);

    fw.force_result = R_NOT_READY;              // a config op: the device's JSON is passed back
    fw.force_message = "busy";
    CHECK(ab_config_get(d, &j) == AB_ERR_API);
    CHECK(!strcmp(ab_jstr(j, "result", ""), "not_ready") && ab_jobj_get(j, "config"));
    cJSON_Delete(j);

    CHECK(ab_config_get(d, &j) == AB_OK);       // "config" member only
    CHECK(ab_jbool(j, "write_protect", false) && !ab_jobj_get(j, "result"));
    cJSON_Delete(j);
    ab_close(d);

    // ok, but no JSON where JSON is due; JSON lacking the member asked for
    uint8_t raw[64];
    d = mkdev(true);
    tp.raw = raw;
    tp.raw_len = mkreply(raw, 1, OP_STATUS, R_OK, CT_NONE, NULL, 0, 0, 0);
    j = (cJSON *)1;
    CHECK(ab_status(d, &j) == AB_ERR_TRANSPORT && j == NULL);
    CHECK(strstr(ab_errmsg(d), "expected JSON") != NULL);
    tp.raw_len = mkreply(raw, 1, OP_CONFIG_GET, R_OK, CT_JSON, "{\"result\":\"ok\"}", 15, 0, 0);
    CHECK(ab_config_get(d, &j) == AB_ERR_TRANSPORT && j == NULL);
    CHECK(strstr(ab_errmsg(d), "lacks") != NULL);
    ab_close(d);
}

static void test_log_paging(void) {
    ab_dev *d = mkdev(true);
    fw.log_len = 12;
    cJSON *j = NULL;
    CHECK(ab_log(d, &j) == AB_OK);
    CHECK(ab_jnum(j, "total", -1) == 12 && ab_jnum(j, "retained", -1) == 12);
    const cJSON *e = ab_jobj_get(j, "entries");
    CHECK(cJSON_GetArraySize(e) == 12);
    for (int i = 0; i < 12; i++) CHECK(ab_jnum(cJSON_GetArrayItem(e, i), "seq", -1) == i + 1);
    cJSON_Delete(j);
    CHECK(fw.runs[OP_LOG] == 3);
    CHECK(ab_be32(fw.seen_args[OP_LOG]) == 10);

    fw.log_len = 0;
    CHECK(ab_log(d, &j) == AB_OK);
    CHECK(cJSON_GetArraySize(ab_jobj_get(j, "entries")) == 0 && fw.runs[OP_LOG] == 4);
    cJSON_Delete(j);

    fw.log_len = 7;                             // a failing page: the device's JSON comes back
    fw.force_result = R_BAD_ARG;
    fw.force_message = "bad start";
    CHECK(ab_log(d, &j) == AB_ERR_API);
    CHECK(!strcmp(ab_jstr(j, "result", ""), "bad_arg"));
    cJSON_Delete(j);
    CHECK(ab_log(d, NULL) == AB_OK);
    ab_close(d);
}

static void test_config_set_values(void) {
    static const struct { const char *key, *value; int rc; uint8_t num; uint32_t v; } cases[] = {
        {"write_protect", "on", AB_OK, KEY_WRITE_PROTECT, 1},
        {"write_protect", "OFF", AB_OK, KEY_WRITE_PROTECT, 0},
        {"auto_mount", "yes", AB_OK, KEY_AUTO_MOUNT, 1},
        {"iordy", "False", AB_OK, KEY_IORDY, 0},
        {"intrq", "enabled", AB_OK, KEY_INTRQ, 1},
        {"no_retry", "disabled", AB_OK, KEY_NO_RETRY, 0},
        {"no_retry", "1", AB_OK, KEY_NO_RETRY, 1},
        {"read_timeout_ms", "1500", AB_OK, KEY_READ_TIMEOUT, 1500},
        {"read_timeout_ms", "0x100", AB_OK, KEY_READ_TIMEOUT, 256},
        {"write_timeout_ms", "600000", AB_OK, KEY_WRITE_TIMEOUT, 600000},
        {"ecc_len", "4294967295", AB_OK, KEY_ECC_LEN, 0xFFFFFFFFu},
        {"device", "slave", AB_OK, KEY_DEVICE, 1},
        {"device", "MASTER", AB_OK, KEY_DEVICE, 0},
        {"device", "1", AB_OK, KEY_DEVICE, 1},
        {"no_retry", "2", AB_ERR_ARG, 0, 0},
        {"no_retry", "", AB_ERR_ARG, 0, 0},
        {"write_protect", "maybe", AB_ERR_ARG, 0, 0},
        {"read_timeout_ms", "-1", AB_ERR_ARG, 0, 0},
        {"read_timeout_ms", "4294967296", AB_ERR_ARG, 0, 0},
        {"read_timeout_ms", "12ms", AB_ERR_ARG, 0, 0},
        {"read_timeout_ms", "", AB_ERR_ARG, 0, 0},
        {"device", "2", AB_ERR_ARG, 0, 0},
        {"device", "cable", AB_ERR_ARG, 0, 0},
        {"bogus", "1", AB_ERR_ARG, 0, 0},
    };
    ab_dev *d = mkdev(true);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned before = tp.ncdb;
        cJSON *j = (cJSON *)1;
        int rc = ab_config_set(d, cases[i].key, cases[i].value, &j);
        if (rc != cases[i].rc) {
            printf("FAIL config_set %s=%s: %d, want %d\n", cases[i].key, cases[i].value, rc, cases[i].rc);
            failures++;
        }
        if (cases[i].rc != AB_OK) {
            CHECK(tp.ncdb == before && j == NULL);       // refused locally, nothing sent
            continue;
        }
        CHECK(j && cJSON_IsObject(j) && !ab_jobj_get(j, "result"));   // the new config object
        cJSON_Delete(j);
        CHECK(tp.ncdb == before + 1);
        const uint8_t *c = tp.cdbs[before];
        CHECK(c[1] == OP_CONFIG_SET && c[4] == cases[i].num && !c[5] && !c[6] && !c[7]);
        CHECK(ab_be32(&c[8]) == cases[i].v);
        CHECK(ab_be32(&c[12]) != 0);
    }
    cJSON *j = NULL;
    CHECK(ab_config_set(d, "no_retry", "on", &j) == AB_OK && ab_jbool(j, "no_retry", false));
    cJSON_Delete(j);
    CHECK(ab_config_set(d, "read_timeout_ms", "10", &j) == AB_ERR_API);   // the device refuses
    CHECK(!strcmp(ab_jstr(j, "result", ""), "bad_arg") && ab_jobj_get(j, "config"));
    CHECK(strstr(ab_errmsg(d), "50..600000") != NULL);
    cJSON_Delete(j);
    CHECK(ab_config_set(d, "read_timeout_ms", "10", NULL) == AB_ERR_API);
    for (int i = 0; ab_config_keys[i]; i++) CHECK(strlen(ab_config_keys[i]) > 0);
    ab_close(d);
}

int main(void) {
    test_op_tables();
    test_ext_detected();
    test_old_firmware_falls_back_to_basic();
    test_check_condition_once_extended();
    test_ids_only_on_state_changing_ops();
    test_rid_on_every_op();
    test_args_overlapping_rid_refused();
    test_empty_reply_read_only_resent();
    test_empty_reply_state_change_not_resent();
    test_empty_reply_state_change_resent_with_same_id();
    test_other_ops_not_resent();
    test_lost_reply();
    test_unsent_request_resent();
    test_two_losses_give_up();
    test_reboot_detected();
    test_reboot_since_seeded_id();
    test_state_counter();
    test_reply_decoding();
    test_reply_errors();
    test_json_op_results();
    test_log_paging();
    test_config_set_values();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all engine tests passed\n");
    return 0;
}
