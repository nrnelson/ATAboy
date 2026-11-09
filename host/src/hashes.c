// HASHES (0x26): SHA-256 of recently read sectors as they came off the IDE
// bus, paged out of the device's ring; comparison against an image.

#include "internal.h"
#include "api_proto.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Reply layout (big-endian; host/ataboy HASH_HDR ">IIIHHHBxQ4x", HASH_REC ">IIBBxx32s")
#define HH_NEWEST    0
#define HH_OLDEST    4
#define HH_NEXT      8
#define HH_COUNT     12
#define HH_REC_LEN   14
#define HH_CAPACITY  16
#define HH_FLAGS     18
#define HH_BUSY_US   20
#define HR_SEQ       0
#define HR_LBA       4
#define HR_SOURCE    8
#define HR_FLAGS     9
#define HR_DIGEST    12

static uint64_t be64(const uint8_t *p) { return ((uint64_t)ab_be32(p) << 32) | ab_be32(p + 4); }

static void hexs(const uint8_t *p, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    out[2 * n] = '\0';
}

int ab_hashes_parse(const uint8_t *p, size_t len, cJSON **head, cJSON **records, uint32_t *next) {
    *head = NULL;
    *records = NULL;
    if (len < HASH_HDR_LEN) return ab_fail(AB_ERR_TRANSPORT, "short HASHES reply (%zu bytes)", len);
    uint32_t newest = ab_be32(p + HH_NEWEST), oldest = ab_be32(p + HH_OLDEST);
    unsigned n = ab_be16(p + HH_COUNT), rec_len = ab_be16(p + HH_REC_LEN), cap = ab_be16(p + HH_CAPACITY);
    uint8_t flags = p[HH_FLAGS];
    uint64_t busy = be64(p + HH_BUSY_US);
    if (rec_len < HASH_REC_LEN || len < HASH_HDR_LEN + (size_t)n * rec_len)
        return ab_fail(AB_ERR_TRANSPORT, "malformed HASHES reply");
    cJSON *recs = cJSON_CreateArray();
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *r = p + HASH_HDR_LEN + (size_t)i * rec_len;
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "seq", ab_be32(r + HR_SEQ));
        cJSON_AddNumberToObject(x, "lba", ab_be32(r + HR_LBA));
        char src[24];
        uint8_t s = r[HR_SOURCE];
        if (s == 1) strcpy(src, "usb");
        else if (s == 2) strcpy(src, "api_read");
        else snprintf(src, sizeof(src), "source_%u", s);
        cJSON_AddStringToObject(x, "source", src);
        cJSON_AddBoolToObject(x, "err_data", r[HR_FLAGS] & 1);
        char hx[65];
        hexs(r + HR_DIGEST, 32, hx);
        cJSON_AddStringToObject(x, "sha256", hx);
        cJSON_AddItemToArray(recs, x);
    }
    cJSON *h = cJSON_CreateObject();
    cJSON_AddNumberToObject(h, "newest", newest);
    cJSON_AddNumberToObject(h, "oldest", oldest);
    cJSON_AddNumberToObject(h, "capacity", cap);
    cJSON_AddBoolToObject(h, "active", flags & 1);
    cJSON_AddNumberToObject(h, "busy_us", (double)busy);
    if (newest) cJSON_AddNumberToObject(h, "us_per_sector", ab_round((double)busy / newest, 2));
    else cJSON_AddNullToObject(h, "us_per_sector");
    *head = h;
    *records = recs;
    *next = ab_be32(p + HH_NEXT);
    return AB_OK;
}

static int hashes_page(ab_dev *dev, uint32_t start, bool clear, cJSON **head, cJSON **recs, uint32_t *next,
                       cJSON **json_out) {
    uint8_t a[5];
    ab_put_be32(a, start);
    a[4] = clear ? 1 : 0;
    ab_reply r;
    int rc = ab_call(dev, OP_HASHES, a, 5, 0, 0, &r);
    if (rc != AB_OK) return rc;
    if (r.ctype != CT_BINARY) {
        const char *m = ab_jstr(r.json, "message", "");
        ab_fail(AB_ERR_API, "%s%s%s", api_result_name(r.result), *m ? ": " : "", m);
        if (cJSON_IsObject(r.json)) { *json_out = r.json; r.json = NULL; }
        else {
            *json_out = cJSON_CreateObject();
            cJSON_AddStringToObject(*json_out, "result", api_result_name(r.result));
        }
        ab_reply_free(&r);
        return ab_dev_fail(dev, AB_ERR_API);
    }
    rc = ab_hashes_parse(r.payload, r.payload_len, head, recs, next);
    ab_reply_free(&r);
    return rc == AB_OK ? AB_OK : ab_dev_fail(dev, rc);
}

int ab_hashes(ab_dev *dev, uint32_t since, bool clear, bool has_lba, uint64_t lba, uint64_t count, cJSON **out) {
    *out = NULL;
    cJSON *head = NULL, *recs = NULL, *jerr = NULL;
    uint32_t next = 0;
    if (clear) {
        int rc = hashes_page(dev, 0, true, &head, &recs, &next, &jerr);
        if (rc != AB_OK) { *out = jerr; return rc; }
        cJSON_Delete(recs);
        cJSON_AddArrayToObject(head, "records");
        cJSON_AddBoolToObject(head, "cleared", true);
        *out = head;
        return AB_OK;
    }
    cJSON *all = cJSON_CreateArray();
    uint32_t start = since;
    for (;;) {
        cJSON_Delete(head);
        head = NULL;
        int rc = hashes_page(dev, start, false, &head, &recs, &next, &jerr);
        if (rc != AB_OK) { cJSON_Delete(all); *out = jerr; return rc; }
        int got = cJSON_GetArraySize(recs);
        cJSON *x;
        while ((x = cJSON_DetachItemFromArray(recs, 0)) != NULL) {
            if (!has_lba) { cJSON_AddItemToArray(all, x); continue; }
            double l = ab_jnum(x, "lba", -1);
            if (l >= (double)lba && l < (double)lba + (double)count) cJSON_AddItemToArray(all, x);
            else cJSON_Delete(x);
        }
        cJSON_Delete(recs);
        recs = NULL;
        if (!got || (double)next > ab_jnum(head, "newest", 0) || next <= start) break;
        start = next;
    }
    cJSON_AddItemToObject(head, "records", all);
    *out = head;
    return AB_OK;
}

int ab_compare_hashes(const cJSON *records, const char *image, cJSON **out) {
    *out = NULL;
    FILE *f = fopen(image, "rb");
    if (!f) return ab_fail(AB_ERR_IO, "cannot open %s: %s", image, strerror(errno));
    unsigned match = 0, beyond = 0;
    cJSON *differ = cJSON_CreateArray();
    for (const cJSON *x = records ? records->child : NULL; x; x = x->next) {
        double l = ab_jnum(x, "lba", -1);
        uint8_t data[512];
        size_t got = 0;
        if (l >= 0 && l <= (double)(UINT64_C(1) << 53) / 512 &&
            ab_fseek(f, (long long)((uint64_t)l * 512), SEEK_SET) == 0)
            got = fread(data, 1, sizeof(data), f);
        if (got < 512) { beyond++; continue; }
        uint8_t dg[32];
        char hx[65];
        ab_sha256(data, 512, dg);
        hexs(dg, 32, hx);
        if (!strcmp(hx, ab_jstr(x, "sha256", ""))) match++;
        else cJSON_AddItemToArray(differ, cJSON_Duplicate(x, true));
    }
    bool err = ferror(f) != 0;
    fclose(f);
    if (err) { cJSON_Delete(differ); return ab_fail(AB_ERR_IO, "cannot read %s", image); }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "image", image);
    cJSON_AddNumberToObject(o, "match", match);
    cJSON_AddItemToObject(o, "differ", differ);
    cJSON_AddNumberToObject(o, "beyond_end", beyond);
    *out = o;
    return AB_OK;
}
