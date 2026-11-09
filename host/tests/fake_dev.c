// A model of the API firmware for tests (see fake_dev.h).

#include "fake_dev.h"
#include "api_proto.h"
#include <stdlib.h>
#include <string.h>

void fake_sector(uint64_t lba, uint8_t out[512]) {
    for (int i = 0; i < 128; i++) ab_put_be32(out + 4 * i, (uint32_t)lba);
}

void fake_ecc(uint64_t lba, unsigned n, uint8_t *out) {
    for (unsigned i = 0; i < n; i++) out[i] = (uint8_t)((lba * 7 + i) & 0xFF);
}

static fk_mode mode_of(const fake_dev *f, uint64_t lba) {
    for (int i = 0; i < f->nbad; i++) if (f->bad[i].lba == lba) return f->bad[i].mode;
    return 0;
}

static fk_mode fails(const fake_dev *f, uint64_t lba, bool no_retry) {
    fk_mode m = mode_of(f, lba);
    if (m == FK_NORETRY && !no_retry) return 0;
    return m;
}

static size_t reply(fake_dev *f, uint8_t *out, uint8_t op, uint8_t result, uint8_t ctype, const uint8_t *payload,
                    size_t plen) {
    memset(out, 0, HDR_LEN);
    out[0] = 'A';
    out[1] = 'B';
    out[2] = API_PROTO_VERSION;
    out[3] = op;
    out[4] = result;
    out[5] = ctype;
    ab_put_be16(out + 6, (uint16_t)plen);
    ab_put_be32(out + 8, f->uptime);
    out[12] = (uint8_t)((f->mounted ? HF_MOUNTED : 0) | HF_WRITE_PROTECT | HF_IDENTIFIED |
                        (f->geometry_valid ? HF_GEOMETRY_VALID : 0));
    if (plen) memmove(out + HDR_LEN, payload, plen);
    return HDR_LEN + plen;
}

static size_t jreply(fake_dev *f, uint8_t *out, uint8_t op, uint8_t result, cJSON *o, const char *message) {
    if (!o) o = cJSON_CreateObject();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "result", api_result_name(result));
    if (message) cJSON_AddStringToObject(r, "message", message);
    for (cJSON *c = o->child; c; c = c->next) cJSON_AddItemToObject(r, c->string, cJSON_Duplicate(c, true));
    cJSON_Delete(o);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    size_t n = reply(f, out, op, result, CT_JSON, (const uint8_t *)s, strlen(s));
    free(s);
    return n;
}

static void result_block(uint8_t *p, uint32_t lba, unsigned count, unsigned done, uint8_t command, int ide,
                         uint8_t status, uint8_t error, bool err_data, bool tf_valid, uint32_t fail_lba,
                         unsigned ecc_len) {
    memset(p, 0, RES_LEN);
    ab_put_be32(p + RES_LBA, lba);
    ab_put_be16(p + RES_COUNT, (uint16_t)count);
    ab_put_be16(p + RES_DONE, (uint16_t)done);
    p[RES_STATUS] = status;
    p[RES_ERROR] = error;
    p[RES_COMMAND] = command;
    p[RES_FLAGS] = (uint8_t)((ide == APIIDE_TIMEOUT ? RF_TIMED_OUT | RF_RECOVERED : 0) | (err_data ? RF_ERR_DATA : 0) |
                             (tf_valid ? RF_TF_VALID : 0));
    ab_put_be32(p + RES_ELAPSED_US, 1234);
    uint32_t fail = ide == 0 ? 0xFFFFFFFFu : fail_lba;
    ab_put_be32(p + RES_FAIL_LBA, fail);
    if (tf_valid) {
        uint8_t tf[5] = {1, (uint8_t)fail, (uint8_t)(fail >> 8), (uint8_t)(fail >> 16), (uint8_t)(0xE0 | ((fail >> 24) & 0xF))};
        memcpy(p + RES_TF, tf, 5);
    } else {
        memset(p + RES_TF, status, 5);
    }
    p[RES_ECC_LEN] = (uint8_t)ecc_len;
    p[RES_IDE_RESULT] = (uint8_t)(int8_t)ide;
}

static void hash_sector(fake_dev *f, uint64_t lba, const uint8_t *data, uint8_t flags) {
    if (f->nhash == FK_MAX_HASH || f->nhash == f->hash_capacity) {
        memmove(&f->hashes[0], &f->hashes[1], (f->nhash - 1) * sizeof(fk_hash));
        f->nhash--;
    }
    fk_hash *h = &f->hashes[f->nhash++];
    h->seq = ++f->hash_newest;
    h->lba = (uint32_t)lba;
    h->source = 2;
    h->flags = flags;
    ab_sha256(data, 512, h->digest);
}

static size_t op_read(fake_dev *f, uint8_t *out, const uint8_t *a, uint32_t alloc) {
    uint32_t lba = ab_be32(a);
    unsigned count = ab_be16(a + 4);
    uint8_t flags = a[6];
    unsigned maxn = (alloc - 16 - 32) / 512;
    if (count == 0 || count > maxn) return jreply(f, out, OP_READ, R_BAD_ARG, NULL, "count must be 1..max_read_sectors");
    if ((uint64_t)lba + count > f->total) return jreply(f, out, OP_READ, R_BAD_ARG, NULL, "beyond end of drive");
    bool no_retry = flags & 1;
    uint8_t cmd = no_retry ? 0x21 : 0x20;
    static uint8_t data[8 * 512];
    size_t dl = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t s = (uint64_t)lba + i;
        fk_mode mode = fails(f, s, no_retry);
        if (!mode) {
            fake_sector(s, data + dl);
            hash_sector(f, s, data + dl, 0);
            dl += 512;
            continue;
        }
        uint8_t res, st, er;
        int ide;
        bool tfv, err_data = false;
        if (mode == FK_TIMEOUT) { res = R_TIMEOUT; ide = APIIDE_TIMEOUT; st = 0x80; er = 0; tfv = false; }
        else {
            res = R_ATA_ERROR; ide = APIIDE_ATA_ERROR; st = 0x59; er = 0x40; tfv = true;
            err_data = (flags & 2) && mode != FK_DEAD;
            if (err_data) {
                fake_sector(s, data + dl);
                for (int k = 0; k < 512; k++) data[dl + (size_t)k] ^= 0xFF;
                hash_sector(f, s, data + dl, 1);
                dl += 512;
            }
        }
        uint8_t blk[RES_LEN + sizeof(data)];
        result_block(blk, lba, count, i, cmd, ide, st, er, err_data, tfv, (uint32_t)s, 0);
        memcpy(blk + RES_LEN, data, dl);
        return reply(f, out, OP_READ, res, CT_BINARY, blk, RES_LEN + dl);
    }
    uint8_t blk[RES_LEN + sizeof(data)];
    result_block(blk, lba, count, count, cmd, 0, 0x50, 0, false, true, 0, 0);
    memcpy(blk + RES_LEN, data, dl);
    return reply(f, out, OP_READ, R_OK, CT_BINARY, blk, RES_LEN + dl);
}

static size_t op_read_long(fake_dev *f, uint8_t *out, const uint8_t *a) {
    uint32_t lba = ab_be32(a);
    unsigned ecc = a[4] ? a[4] : f->ecc;
    uint8_t cmd = (a[6] & 1) ? 0x23 : 0x22;
    if (lba >= f->total) return jreply(f, out, OP_READ_LONG, R_BAD_ARG, NULL, "beyond end of drive");
    if (lba >= (1u << 28)) return jreply(f, out, OP_READ_LONG, R_BAD_ARG, NULL, "READ LONG needs a CHS/LBA28 address");
    fk_mode mode = mode_of(f, lba);
    uint8_t blk[RES_LEN + 512 + 255];
    if (mode == FK_DEAD || mode == FK_TIMEOUT) {
        bool to = mode == FK_TIMEOUT;
        result_block(blk, lba, 1, 0, cmd, to ? APIIDE_TIMEOUT : APIIDE_ATA_ERROR, 0x51, 0x10, false, !to, lba, ecc);
        return reply(f, out, OP_READ_LONG, to ? R_TIMEOUT : R_ATA_ERROR, CT_BINARY, blk, RES_LEN);
    }
    unsigned n = 0;
    for (int i = 0; i < f->nflaky; i++) if (f->flaky[i] == lba) n = ++f->flaky_count[i];
    result_block(blk, lba, 1, 1, cmd, 0, 0x50, 0, false, true, 0, ecc);
    fake_sector(lba, blk + RES_LEN);
    if (n && n % 2 == 0) blk[RES_LEN] ^= 0x55;
    fake_ecc(lba, ecc, blk + RES_LEN + 512);
    return reply(f, out, OP_READ_LONG, R_OK, CT_BINARY, blk, RES_LEN + 512 + ecc);
}

static size_t op_hashes(fake_dev *f, uint8_t *out, const uint8_t *a, uint32_t alloc) {
    uint32_t start = ab_be32(a);
    if (a[4] & 1) { f->nhash = 0; f->hash_newest = 0; }
    uint32_t oldest = f->nhash ? f->hashes[0].seq : 0;
    if (start < oldest) start = oldest;
    size_t fit = (alloc - 16 - 32) / 44;
    static uint8_t body[4096];
    uint8_t *p = body + HASH_HDR_LEN;
    size_t n = 0;
    uint32_t last = 0;
    for (size_t i = 0; i < f->nhash && n < fit; i++) {
        const fk_hash *h = &f->hashes[i];
        if (h->seq < start) continue;
        memset(p, 0, HASH_REC_LEN);
        ab_put_be32(p, h->seq);
        ab_put_be32(p + 4, h->lba);
        p[8] = h->source;
        p[9] = h->flags;
        memcpy(p + 12, h->digest, 32);
        p += HASH_REC_LEN;
        last = h->seq;
        n++;
    }
    uint32_t next = n ? last + 1 : (start > f->hash_newest ? start : f->hash_newest + 1);
    memset(body, 0, HASH_HDR_LEN);
    ab_put_be32(body, f->hash_newest);
    ab_put_be32(body + 4, oldest);
    ab_put_be32(body + 8, next);
    ab_put_be16(body + 12, (uint16_t)n);
    ab_put_be16(body + 14, HASH_REC_LEN);
    ab_put_be16(body + 16, (uint16_t)f->hash_capacity);
    body[18] = 1;
    uint64_t busy = 10ull * f->hash_newest;
    ab_put_be32(body + 20, (uint32_t)(busy >> 32));
    ab_put_be32(body + 24, (uint32_t)busy);
    return reply(f, out, OP_HASHES, R_OK, CT_BINARY, body, HASH_HDR_LEN + n * HASH_REC_LEN);
}

static int fk_api(ab_transport *t, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms, uint8_t *out,
                  size_t *out_len, bool *maybe_executed, uint8_t sense[3]) {
    (void)timeout_ms;
    fake_dev *f = (fake_dev *)t;
    *out_len = 0;
    *maybe_executed = false;
    if (cdb[0] != API_OPCODE || cdb[2] != API_SIG0 || cdb[3] != API_SIG_BASIC || alloc < API_MIN_ALLOC) {
        sense[0] = 5; sense[1] = 0x24; sense[2] = 0;
        return AB_ERR_CHECK;
    }
    uint8_t op = cdb[1];
    const uint8_t *a = cdb + 4;
    if (f->ncalls == f->capcalls) {
        f->capcalls = f->capcalls ? f->capcalls * 2 : 64;
        f->calls = realloc(f->calls, f->capcalls * sizeof(fk_call));
        if (!f->calls) abort();
    }
    f->calls[f->ncalls].op = op;
    memcpy(f->calls[f->ncalls].a, a, 12);
    f->ncalls++;
    if (f->hook) {
        int e = f->hook(f, op, a, sense);
        if (e) { *maybe_executed = true; return e; }
    }
    f->uptime += 10;
    static uint8_t buf[8192];
    size_t n;
    if (op == OP_INFO) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "proto", 1);
        cJSON_AddStringToObject(o, "fw", "fake-api1");
        cJSON_AddNumberToObject(o, "max_read_sectors", 7);
        n = jreply(f, buf, op, R_OK, o, NULL);
    } else if (op == OP_STATUS) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "mounted", f->mounted);
        cJSON *g = cJSON_AddObjectToObject(o, "geometry");
        cJSON_AddBoolToObject(g, "valid", f->geometry_valid);
        cJSON_AddNumberToObject(g, "sectors", (double)f->total);
        cJSON *c = cJSON_AddObjectToObject(o, "config");
        cJSON_AddNumberToObject(c, "read_timeout_ms", 1000);
        n = jreply(f, buf, op, R_OK, o, NULL);
    } else if (op == OP_READ) {
        n = op_read(f, buf, a, alloc);
    } else if (op == OP_READ_LONG) {
        n = op_read_long(f, buf, a);
    } else if (op == OP_HASHES) {
        n = op_hashes(f, buf, a, alloc);
    } else {
        n = jreply(f, buf, op, R_BAD_SUBOP, NULL, "unknown sub-operation");
    }
    if (n > alloc) { fprintf(stderr, "fake: reply %zu > allocation %u\n", n, (unsigned)alloc); abort(); }
    memcpy(out, buf, n);
    *out_len = n;
    return AB_OK;
}

static void fk_close(ab_transport *t) { (void)t; }

void fake_init(fake_dev *f, uint64_t total) {
    memset(f, 0, sizeof(*f));
    f->tp.name = "fake";
    f->tp.api = fk_api;
    f->tp.close = fk_close;
    f->total = total;
    f->ecc = 4;
    f->geometry_valid = true;
    f->uptime = 1000;
    f->hash_capacity = 1024;
}

void fake_free(fake_dev *f) { free(f->calls); f->calls = NULL; f->ncalls = f->capcalls = 0; }

void fake_bad(fake_dev *f, uint64_t lba, fk_mode mode) {
    if (f->nbad < FK_MAX_BAD) { f->bad[f->nbad].lba = lba; f->bad[f->nbad].mode = mode; f->nbad++; }
}

void fake_flaky(fake_dev *f, uint64_t lba) {
    if (f->nflaky < 8) f->flaky[f->nflaky++] = lba;
}

unsigned fake_readlong_count(const fake_dev *f, uint64_t lba) {
    for (int i = 0; i < f->nflaky; i++) if (f->flaky[i] == lba) return f->flaky_count[i];
    return 0;
}

ab_dev *fake_open(fake_dev *f) {
    ab_dev *d = calloc(1, sizeof(*d));
    if (!d) abort();
    d->tp = &f->tp;
    d->ext = -1;
    snprintf(d->path, sizeof(d->path), "/fake");
    return d;
}
