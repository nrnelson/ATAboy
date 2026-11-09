// Fake API firmware for the host tests (see fakefw.h).
#include "fakefw.h"
#include "api_proto.h"
#include "rid_cache.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static void put_be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void fakefw_init(fakefw *fw, bool ext) {
    memset(fw, 0, sizeof(*fw));
    fw->ext = ext;
    fw->boot_id = 0x5EED0001u;
    fw->uptime = 1000;
    fw->write_protect = true;
    fw->read_timeout_ms = 1000;
    fw->log_page = 5;
    fw->force_result = -1;
    rid_clear();
}

void fakefw_reboot(fakefw *fw) {
    fw->boot_id++;
    fw->state_counter = 0;
    fw->mounted = false;
    fw->uptime = 500;
    rid_clear();
}

static uint8_t hdr_flags(const fakefw *fw) {
    return (uint8_t)((fw->mounted ? HF_MOUNTED : 0) | (fw->write_protect ? HF_WRITE_PROTECT : 0) |
                     HF_IDENTIFIED | HF_GEOMETRY_VALID);
}

// The basic 16-byte header (firmware finish()).
static int finish(const fakefw *fw, uint8_t *buf, uint8_t op, int result, uint8_t ctype, uint32_t plen) {
    buf[0] = 'A'; buf[1] = 'B';
    buf[2] = API_PROTO_VERSION;
    buf[3] = op;
    buf[4] = (uint8_t)result;
    buf[5] = ctype;
    put_be16(&buf[6], plen);
    put_be32(&buf[8], fw->uptime);
    buf[12] = hdr_flags(fw);
    buf[13] = buf[14] = buf[15] = 0;
    return (int)(HDR_LEN + plen);
}

static cJSON *jres(int result, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", api_result_name(result));
    if (msg) cJSON_AddStringToObject(o, "message", msg);
    return o;
}

static int finish_json(const fakefw *fw, uint8_t *buf, uint32_t cap, uint8_t op, int result, cJSON *o) {
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    size_t n = s ? strlen(s) : 0;
    if (!s || HDR_LEN + n > cap) { free(s); return finish(fw, buf, op, R_ALLOC_TOO_SMALL, CT_NONE, 0); }
    memcpy(buf + HDR_LEN, s, n);
    free(s);
    return finish(fw, buf, op, result, CT_JSON, (uint32_t)n);
}

static void add_config(const fakefw *fw, cJSON *o) {
    cJSON *c = cJSON_AddObjectToObject(o, "config");
    cJSON_AddBoolToObject(c, "write_protect", fw->write_protect);
    cJSON_AddBoolToObject(c, "no_retry", fw->no_retry);
    cJSON_AddNumberToObject(c, "read_timeout_ms", fw->read_timeout_ms);
    cJSON_AddStringToObject(c, "device", fw->device ? "slave" : "master");
}

static void add_status(const fakefw *fw, cJSON *o) {
    cJSON_AddBoolToObject(o, "mounted", fw->mounted);
    add_config(fw, o);
}

// run_op(): one request with the basic header, built at buf.
static int run_op(fakefw *fw, const uint8_t *req, uint8_t *buf, uint32_t cap) {
    uint8_t op = req[1];
    const uint8_t *a = req + 4;
    fw->runs[op]++;
    memcpy(fw->seen_args[op], a, 12);
    fw->uptime += 10;
    if (api_op_takes_rid(op)) fw->state_counter++;
    if (fw->force_result >= 0) {
        int res = fw->force_result;
        fw->force_result = -1;
        if (fw->force_no_payload) { fw->force_no_payload = false; return finish(fw, buf, op, res, CT_NONE, 0); }
        cJSON *o = jres(res, fw->force_message);
        add_status(fw, o);
        return finish_json(fw, buf, cap, op, res, o);
    }
    cJSON *o;
    switch (op) {
    case OP_INFO:
        o = jres(R_OK, NULL);
        cJSON_AddNumberToObject(o, "proto", API_PROTO_VERSION);
        cJSON_AddStringToObject(o, "fw", "fake-api1");
        cJSON_AddNumberToObject(o, "max_read_sectors", 7);
        return finish_json(fw, buf, cap, op, R_OK, o);
    case OP_STATUS:
        o = jres(R_OK, NULL);
        add_status(fw, o);
        return finish_json(fw, buf, cap, op, R_OK, o);
    case OP_CONFIG_GET:
        o = jres(R_OK, NULL);
        add_config(fw, o);
        return finish_json(fw, buf, cap, op, R_OK, o);
    case OP_CONFIG_SET: {
        uint32_t v = be32(&a[4]);
        int res = R_OK;
        const char *msg = NULL;
        switch (a[0]) {
        case KEY_WRITE_PROTECT: fw->write_protect = v != 0; break;
        case KEY_NO_RETRY: fw->no_retry = v != 0; break;
        case KEY_DEVICE:
            if (v > 1) { res = R_BAD_ARG; msg = "device must be 0 or 1"; } else fw->device = v;
            break;
        case KEY_READ_TIMEOUT:
            if (v < 50 || v > 600000) { res = R_BAD_ARG; msg = "timeout must be 50..600000 ms"; }
            else fw->read_timeout_ms = v;
            break;
        case KEY_AUTO_MOUNT: case KEY_IORDY: case KEY_INTRQ: case KEY_WRITE_TIMEOUT: case KEY_ECC_LEN:
            break;
        default:
            res = R_BAD_ARG; msg = "unknown config key";
        }
        o = jres(res, msg);
        add_config(fw, o);
        return finish_json(fw, buf, cap, op, res, o);
    }
    case OP_MOUNT:
        fw->mounted = true;
        o = jres(R_OK, NULL);
        add_status(fw, o);
        return finish_json(fw, buf, cap, op, R_OK, o);
    case OP_UNMOUNT:
        fw->mounted = false;
        o = jres(R_OK, NULL);
        add_status(fw, o);
        return finish_json(fw, buf, cap, op, R_OK, o);
    case OP_LOG: {
        uint32_t start = be32(&a[0]), i = start;
        o = jres(R_OK, NULL);
        cJSON_AddNumberToObject(o, "total", fw->log_len);
        cJSON_AddNumberToObject(o, "retained", fw->log_len);
        cJSON_AddNumberToObject(o, "start", start);
        cJSON *e = cJSON_AddArrayToObject(o, "entries");
        for (; i < fw->log_len && i < start + fw->log_page; i++) {
            cJSON *x = cJSON_CreateObject();
            cJSON_AddNumberToObject(x, "seq", i + 1);
            cJSON_AddItemToArray(e, x);
        }
        cJSON_AddNumberToObject(o, "returned", i - start);
        if (i < fw->log_len) cJSON_AddNumberToObject(o, "next", i);
        else cJSON_AddNullToObject(o, "next");
        return finish_json(fw, buf, cap, op, R_OK, o);
    }
    case OP_LOG_CLEAR:
        if (a[0] & 1) fw->log_len = 0;
        return finish_json(fw, buf, cap, op, R_OK, jres(R_OK, NULL));
    default:
        return finish_json(fw, buf, cap, op, R_BAD_SUBOP, jres(R_BAD_SUBOP, "unknown sub-op"));
    }
}

static int wrap_ext(const fakefw *fw, uint8_t *buf, int n, uint32_t rid, uint8_t xflags) {
    memcpy(buf, buf + HDR_LEN, HDR_LEN);      // the payload is already at EXT_HDR_LEN
    buf[13] = EXT_HDR_LEN;
    buf[14] = buf[15] = 0;
    put_be32(&buf[16], fw->boot_id);
    put_be32(&buf[20], fw->state_counter);
    put_be32(&buf[24], rid);
    buf[28] = xflags;
    buf[29] = buf[30] = buf[31] = 0;
    return n + HDR_LEN;
}

int fakefw_dispatch(fakefw *fw, const uint8_t cdb[16], uint32_t cap, uint8_t *out, uint8_t sense[3]) {
    bool ext = fw->ext && cdb[3] == API_SIG_EXT;
    if (cdb[0] != API_OPCODE || cdb[2] != API_SIG0 || (cdb[3] != API_SIG_BASIC && !ext) || cap < API_MIN_ALLOC) {
        sense[0] = 5; sense[1] = 0x24; sense[2] = 0x00;     // ILLEGAL REQUEST, invalid field in CDB
        return -1;
    }
    if (cap > API_MAX_REPLY) cap = API_MAX_REPLY;
    if (!ext) return run_op(fw, cdb, out, cap);

    uint8_t req[16];
    memcpy(req, cdb, 16);
    uint32_t rid = 0;
    uint8_t xflags = 0;
    if (api_op_takes_rid(cdb[1])) {
        rid = be32(&cdb[12]);
        memset(&req[12], 0, 4);                             // the op never sees the id
        xflags = XF_RID;
    }
    const uint8_t *stored;
    uint32_t stored_len;
    switch (rid ? rid_lookup(rid, cdb, &stored, &stored_len) : RID_MISS) {
    case RID_HIT:
        fw->replays++;
        memcpy(out, stored, stored_len);
        put_be32(&out[8], fw->uptime);
        out[12] = hdr_flags(fw);
        put_be32(&out[20], fw->state_counter);
        out[28] |= XF_REPLAYED;
        return (int)stored_len;
    case RID_CONFLICT: {
        int n = finish_json(fw, out + HDR_LEN, cap - HDR_LEN, cdb[1], R_ID_CONFLICT,
                            jres(R_ID_CONFLICT, "request id already used for a different request"));
        return wrap_ext(fw, out, n, rid, xflags);
    }
    default:
        break;
    }
    // Build the basic reply HDR_LEN bytes in, then widen the header.
    int n = run_op(fw, req, out + HDR_LEN, cap - HDR_LEN);
    n = wrap_ext(fw, out, n, rid, xflags);
    if (rid) rid_store(rid, cdb, out, (uint32_t)n);
    return n;
}
