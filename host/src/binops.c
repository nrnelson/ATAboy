// Operations with binary replies: IDENTIFY and the ATA result block of
// READ / READ_LONG / VERIFY / CORRUPT.

#include "internal.h"
#include "api_proto.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

const char *ab_status_bits(uint8_t st, char *buf) { return ata_status_bits(st, buf); }
const char *ab_error_bits(uint8_t er, char *buf) { return ata_error_bits(er, buf); }

// ---------------------------------------------------------------------------
//  IDENTIFY
// ---------------------------------------------------------------------------

int ab_identify(ab_dev *dev, bool cached, uint8_t raw[512]) {
    ab_reply r;
    uint8_t a[1] = {cached ? 1 : 0};
    int rc = ab_call(dev, OP_IDENTIFY, a, 1, 0, 0, &r);
    if (rc != AB_OK) return rc;
    if (r.result != R_OK || r.ctype != CT_BINARY || r.payload_len < 512) {
        const char *m = ab_jstr(r.json, "message", "");
        ab_fail(AB_ERR_API, "%s%s%s", api_result_name(r.result), *m ? ": " : "", m);
        ab_reply_free(&r);
        return ab_dev_fail(dev, AB_ERR_API);
    }
    memcpy(raw, r.payload, 512);
    ab_reply_free(&r);
    return AB_OK;
}

static uint16_t w(const uint8_t *raw, int i) { return (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)); }

// ATA strings: words lo..hi-1, high byte first, trimmed.
static void ata_string(const uint8_t *raw, int lo, int hi, char *out, size_t n) {
    size_t k = 0;
    for (int i = lo; i < hi && k + 2 < n; i++) {
        uint16_t v = w(raw, i);
        char c1 = (char)(v >> 8), c2 = (char)(v & 0xFF);
        out[k++] = (c1 >= 32 && c1 < 127) ? c1 : (c1 ? '?' : ' ');
        out[k++] = (c2 >= 32 && c2 < 127) ? c2 : (c2 ? '?' : ' ');
    }
    out[k] = '\0';
    while (k && out[k - 1] == ' ') out[--k] = '\0';
    size_t s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, strlen(out + s) + 1);
}

static bool sup(const uint8_t *raw, int word, int bit) {
    return w(raw, word) != 0xFFFF && (w(raw, word) & (1u << bit));
}

cJSON *ab_decode_identify(const uint8_t raw[512]) {
    cJSON *d = cJSON_CreateObject();
    char s[64];
    uint32_t lba28 = (uint32_t)w(raw, 60) | ((uint32_t)w(raw, 61) << 16);
    uint64_t lba48 = (uint64_t)w(raw, 100) | ((uint64_t)w(raw, 101) << 16) | ((uint64_t)w(raw, 102) << 32) |
                     ((uint64_t)w(raw, 103) << 48);
    uint16_t major = (w(raw, 80) == 0 || w(raw, 80) == 0xFFFF) ? 0 : w(raw, 80);
    uint64_t chs = (uint64_t)w(raw, 1) * w(raw, 3) * w(raw, 6);
    ata_string(raw, 27, 47, s, sizeof(s)); cJSON_AddStringToObject(d, "model", s);
    ata_string(raw, 10, 20, s, sizeof(s)); cJSON_AddStringToObject(d, "serial", s);
    ata_string(raw, 23, 27, s, sizeof(s)); cJSON_AddStringToObject(d, "firmware", s);
    cJSON *a = cJSON_AddArrayToObject(d, "default_chs");
    cJSON_AddItemToArray(a, cJSON_CreateNumber(w(raw, 1)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(w(raw, 3)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(w(raw, 6)));
    cJSON_AddNumberToObject(d, "default_chs_sectors", (double)chs);
    if (w(raw, 53) & 1) {
        a = cJSON_AddArrayToObject(d, "current_chs");
        for (int i = 54; i <= 56; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(w(raw, i)));
    } else {
        cJSON_AddNullToObject(d, "current_chs");
    }
    bool lba = (w(raw, 49) & 0x200) != 0, l48 = sup(raw, 83, 10);
    cJSON_AddBoolToObject(d, "lba", lba);
    cJSON_AddNumberToObject(d, "lba28_sectors", lba28);
    cJSON_AddBoolToObject(d, "lba48", l48);
    cJSON_AddNumberToObject(d, "lba48_sectors", (double)lba48);
    uint64_t cap = (l48 && lba48) ? lba48 : lba ? lba28 : chs;
    cJSON_AddNumberToObject(d, "capacity_bytes", (double)(cap * 512));
    cJSON_AddNumberToObject(d, "ecc_bytes", w(raw, 22));
    cJSON_AddNumberToObject(d, "buffer_kib", w(raw, 21) / 2);
    cJSON_AddNumberToObject(d, "max_multiple", w(raw, 47) & 0xFF);
    cJSON_AddBoolToObject(d, "iordy", (w(raw, 49) & 0x800) != 0);
    cJSON_AddBoolToObject(d, "dma", (w(raw, 49) & 0x100) != 0);
    a = cJSON_AddArrayToObject(d, "pio_modes");
    cJSON_AddItemToArray(a, cJSON_CreateString("PIO0-2"));
    if (w(raw, 53) & 2) {
        if (w(raw, 64) & 1) cJSON_AddItemToArray(a, cJSON_CreateString("PIO3"));
        if (w(raw, 64) & 2) cJSON_AddItemToArray(a, cJSON_CreateString("PIO4"));
    }
    a = cJSON_AddArrayToObject(d, "udma_modes");
    if (w(raw, 53) & 4)
        for (int i = 0; i < 8; i++) if (w(raw, 88) & (1u << i)) cJSON_AddItemToArray(a, cJSON_CreateNumber(i));
    a = cJSON_AddArrayToObject(d, "ata_versions");
    for (int n = 1; n < 15; n++) if (major & (1u << n)) cJSON_AddItemToArray(a, cJSON_CreateNumber(n));
    cJSON_AddBoolToObject(d, "smart", sup(raw, 82, 0));
    cJSON_AddBoolToObject(d, "smart_enabled", sup(raw, 85, 0));
    cJSON_AddBoolToObject(d, "security", sup(raw, 82, 1));
    cJSON_AddNumberToObject(d, "security_status", w(raw, 128));
    cJSON_AddBoolToObject(d, "write_cache", sup(raw, 82, 5));
    cJSON_AddBoolToObject(d, "write_cache_enabled", sup(raw, 85, 5));
    cJSON_AddBoolToObject(d, "hpa", sup(raw, 82, 10));
    if ((w(raw, 255) & 0xFF) == 0xA5) {
        unsigned sum = 0;
        for (int i = 0; i < 512; i++) sum += raw[i];
        cJSON_AddBoolToObject(d, "integrity_ok", (sum & 0xFF) == 0);
    } else {
        cJSON_AddNullToObject(d, "integrity_ok");
    }
    return d;
}

// ---------------------------------------------------------------------------
//  ATA result block
// ---------------------------------------------------------------------------

static int parse_result(ab_dev *dev, ab_reply *r, bool verify, ab_ataresult *a, cJSON **json_out) {
    if (json_out) *json_out = NULL;
    if (r->ctype != CT_BINARY) {
        const char *m = ab_jstr(r->json, "message", "");
        ab_fail(AB_ERR_API, "%s%s%s", api_result_name(r->result), *m ? ": " : "", m);
        if (json_out) { *json_out = r->json; r->json = NULL; }
        return ab_dev_fail(dev, AB_ERR_API);
    }
    const uint8_t *p = r->payload;
    if (r->payload_len < RES_LEN)
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "short ATA result block (%zu bytes)", r->payload_len));
    memset(a, 0, sizeof(*a));
    a->result = r->result;
    a->lba = ab_be32(p + RES_LBA);
    a->count = ab_be16(p + RES_COUNT);
    a->done = ab_be16(p + RES_DONE);
    a->status = p[RES_STATUS];
    a->error = p[RES_ERROR];
    a->command = p[RES_COMMAND];
    a->flags = p[RES_FLAGS];
    a->elapsed_us = ab_be32(p + RES_ELAPSED_US);
    uint32_t fl = ab_be32(p + RES_FAIL_LBA);
    a->has_fail_lba = fl != 0xFFFFFFFFu;
    a->fail_lba = fl;
    memcpy(a->tf, p + RES_TF, 5);
    a->ecc_len = p[RES_ECC_LEN];
    a->ide_result = (int8_t)p[RES_IDE_RESULT];
    // VERIFY carries a 32-bit count.  The command byte alone isn't enough to
    // recognise it: it is 0 when the firmware gave up before issuing one.
    if (verify || a->command == 0x40 || a->command == 0x41 || a->command == 0x42)
        a->done = ab_be32(p + RES_DONE32);
    a->data = p + RES_LEN;
    a->data_len = r->payload_len - RES_LEN;
    if (a->result == R_OK) return AB_OK;
    ab_fail(AB_ERR_API, "%s", api_result_name(a->result));
    return ab_dev_fail(dev, AB_ERR_API);
}

cJSON *ab_ataresult_json(const ab_ataresult *a) {
    char b[96];
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "result", api_result_name(a->result));
    cJSON_AddStringToObject(d, "ide_result", api_ide_result_name(a->ide_result));
    cJSON_AddNumberToObject(d, "lba", a->lba);
    cJSON_AddNumberToObject(d, "count", a->count);
    cJSON_AddNumberToObject(d, "done", a->done);
    cJSON_AddNumberToObject(d, "command", a->command);
    cJSON_AddNumberToObject(d, "status", a->status);
    cJSON_AddStringToObject(d, "status_bits", ata_status_bits(a->status, b));
    cJSON_AddNumberToObject(d, "error", a->error);
    cJSON_AddStringToObject(d, "error_bits", ata_error_bits(a->error, b));
    cJSON_AddBoolToObject(d, "timed_out", a->flags & RF_TIMED_OUT);
    cJSON_AddBoolToObject(d, "err_data", a->flags & RF_ERR_DATA);
    cJSON_AddBoolToObject(d, "tf_valid", a->flags & RF_TF_VALID);
    cJSON_AddBoolToObject(d, "recovered", a->flags & RF_RECOVERED);
    cJSON_AddBoolToObject(d, "bus_fault", a->flags & RF_BUS_FAULT);
    cJSON_AddBoolToObject(d, "not_ready", a->flags & RF_NOT_READY);
    cJSON_AddBoolToObject(d, "partial", a->flags & RF_PARTIAL);
    if (a->has_fail_lba) cJSON_AddNumberToObject(d, "fail_lba", a->fail_lba);
    else cJSON_AddNullToObject(d, "fail_lba");
    // round(us / 1000, 3) as Python prints it: exact thousandths
    cJSON_AddNumberToObject(d, "elapsed_ms", a->elapsed_us / 1000.0);
    if (a->flags & RF_TF_VALID) {
        cJSON *t = cJSON_AddArrayToObject(d, "tf");
        for (int i = 0; i < 5; i++) cJSON_AddItemToArray(t, cJSON_CreateNumber(a->tf[i]));
    } else {
        cJSON_AddNullToObject(d, "tf");
    }
    cJSON_AddNumberToObject(d, "data_bytes", (double)a->data_len);
    if (a->ecc_len) cJSON_AddNumberToObject(d, "ecc_len", a->ecc_len);
    return d;
}

int ab_read(ab_dev *dev, uint32_t lba, unsigned count, bool no_retry, bool through_error, uint32_t timeout_ms,
            ab_ataresult *a, ab_reply *r, cJSON **json_out) {
    if (count < 1 || count > 8) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "count must be 1..max_read_sectors"));
    uint8_t args[12] = {0};
    ab_put_be32(&args[0], lba);
    ab_put_be16(&args[4], (uint16_t)count);
    args[6] = (uint8_t)((no_retry ? 1 : 0) | (through_error ? 2 : 0));
    ab_put_be32(&args[8], timeout_ms);
    // The firmware enforces the real per-sector timeout; the host's only has to
    // be longer.  Without an explicit value assume the 600 s maximum.
    uint32_t t = (timeout_ms ? timeout_ms : 600000) * count + 15000;
    int rc = ab_call(dev, OP_READ, args, 12, 4096, t, r);
    if (rc != AB_OK) return rc;
    return parse_result(dev, r, false, a, json_out);
}

int ab_read_long(ab_dev *dev, uint32_t lba, unsigned ecc_len, bool no_retry, uint32_t timeout_ms,
                 ab_ataresult *a, ab_reply *r, cJSON **json_out) {
    if (ecc_len > 255) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "ecc_len out of range"));
    uint8_t args[12] = {0};
    ab_put_be32(&args[0], lba);
    args[4] = (uint8_t)ecc_len;
    args[6] = no_retry ? 1 : 0;
    ab_put_be32(&args[8], timeout_ms);
    int rc = ab_call(dev, OP_READ_LONG, args, 12, 4096, (timeout_ms ? timeout_ms : 600000) + 15000, r);
    if (rc != AB_OK) return rc;
    return parse_result(dev, r, false, a, json_out);
}

int ab_verify(ab_dev *dev, uint32_t lba, uint32_t count, uint32_t timeout_ms,
              ab_ataresult *a, ab_reply *r, cJSON **json_out) {
    uint8_t args[12];
    ab_put_be32(&args[0], lba);
    ab_put_be32(&args[4], count);
    ab_put_be32(&args[8], timeout_ms);
    int rc = ab_call(dev, OP_VERIFY, args, 12, 4096, 10000 + (timeout_ms ? timeout_ms : 600000) + 30000, r);
    if (rc != AB_OK) return rc;
    return parse_result(dev, r, true, a, json_out);
}

int ab_corrupt(ab_dev *dev, uint32_t lba, unsigned bits, unsigned ecc_len,
               ab_ataresult *a, ab_reply *r, cJSON **json_out) {
    if (bits < 1 || bits > 255 || ecc_len > 255)
        return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "bits must be 1..255"));
    cJSON *armj = NULL;
    int rc = ab_arm(dev, &armj);
    cJSON_Delete(armj);
    if (rc != AB_OK) return rc;
    uint8_t args[6];
    ab_put_be32(&args[0], lba);
    args[4] = (uint8_t)bits;
    args[5] = (uint8_t)ecc_len;
    rc = ab_call(dev, OP_CORRUPT, args, 6, 4096, 60000, r);
    if (rc != AB_OK) return rc;
    return parse_result(dev, r, false, a, json_out);
}
