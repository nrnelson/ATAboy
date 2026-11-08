// Multi-step operations: ranged reads, VERIFY scans, READ LONG voting.

#include "internal.h"
#include "api_proto.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double ab_round(double v, int digits) {
    double m = pow(10.0, digits);
    return round(v * m) / m;
}

static double seconds_since(uint64_t t0) { return (double)(ab_now_ms() - t0) / 1000.0; }

static int max_read_sectors(ab_dev *dev) {
    if (!dev->max_read_sectors) {
        cJSON *j = NULL;
        if (ab_info(dev, &j) != AB_OK) dev->max_read_sectors = 7;
        cJSON_Delete(j);
    }
    int n = dev->max_read_sectors;
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

static FILE *open_out(const char *path, bool in_place) {
    FILE *f = NULL;
    if (in_place) f = fopen(path, "r+b");
    if (!f) f = fopen(path, "w+b");
    return f;
}

int ab_read_range(ab_dev *dev, uint32_t lba, uint32_t count, const ab_read_range_opts *o_in, cJSON **out) {
    ab_read_range_opts o = {0};
    if (o_in) o = *o_in;
    *out = NULL;
    if ((uint64_t)lba + count > 0xFFFFFFFFull) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "range beyond LBA 2^32"));
    int maxn = max_read_sectors(dev);
    uint64_t t0 = ab_now_ms();
    uint32_t end = lba + count, cur = lba;
    uint64_t ok_sectors = 0;
    cJSON *chunks = cJSON_CreateArray(), *bad = cJSON_CreateArray();
    int nbad = 0, nchunks = 0;
    uint8_t *data = NULL;
    size_t dlen = 0, dcap = 0;
    FILE *f = NULL;
    int rc = AB_OK;
    if (o.out_path) {
        f = open_out(o.out_path, o.image);
        if (!f) { rc = ab_dev_fail(dev, ab_fail(AB_ERR_IO, "cannot open %s", o.out_path)); goto fail; }
    }
    while (cur < end) {
        if (o.time_budget_s > 0 && seconds_since(t0) > o.time_budget_s) break;
        unsigned n = nbad ? 1 : (unsigned)((end - cur) < (uint32_t)maxn ? (end - cur) : (uint32_t)maxn);
        ab_ataresult a;
        ab_reply r;
        cJSON *jerr = NULL;
        int e = ab_read(dev, cur, n, o.no_retry, o.through_error, o.timeout_ms, &a, &r, &jerr);
        if (e != AB_OK && e != AB_ERR_API) { ab_reply_free(&r); rc = e; goto fail; }
        if (jerr) {                            // JSON instead of a result block: refused outright
            ab_reply_free(&r);
            cJSON_Delete(chunks); cJSON_Delete(bad); free(data);
            if (f) fclose(f);
            *out = jerr;
            return AB_ERR_API;
        }
        size_t got = a.done + ((a.flags & RF_ERR_DATA) ? 1 : 0);
        if (got * 512 > a.data_len) got = a.data_len / 512;
        if (f && got) {
            uint64_t off = (uint64_t)(o.image ? cur : cur - lba) * 512;
            if (ab_fseek(f, (long long)off, SEEK_SET) != 0 || fwrite(a.data, 512, got, f) != got) {
                ab_reply_free(&r);
                rc = ab_dev_fail(dev, ab_fail(AB_ERR_IO, "write to %s failed", o.out_path));
                goto fail;
            }
        }
        if (o.keep_data && got) {
            if (dlen + got * 512 > dcap) {
                size_t nc = dcap ? dcap * 2 : 65536;
                while (nc < dlen + got * 512) nc *= 2;
                uint8_t *nd = realloc(data, nc);
                if (!nd) { ab_reply_free(&r); rc = ab_dev_fail(dev, ab_fail(AB_ERR_NOMEM, "out of memory")); goto fail; }
                data = nd;
                dcap = nc;
            }
            memcpy(data + dlen, a.data, got * 512);
            dlen += got * 512;
        }
        ok_sectors += a.done;
        cJSON_AddItemToArray(chunks, ab_ataresult_json(&a));
        nchunks++;
        bool ok = a.result == R_OK;
        ab_reply_free(&r);
        if (ok) { cur += n; continue; }
        // Don't trust a drive-reported LBA outside the request (CHS decoding,
        // stale registers): fall back to the first sector not done.
        uint32_t fl = a.fail_lba;
        if (!a.has_fail_lba || fl < cur || fl >= cur + n) fl = cur + (a.done < n - 1 ? a.done : n - 1);
        cJSON_AddItemToArray(bad, cJSON_CreateNumber(fl));
        nbad++;
        if (!o.keep_going) { cur = fl; break; }
        uint32_t step = a.done ? a.done : 1;
        cur = fl + 1 > cur + step ? fl + 1 : cur + step;
    }
    if (f && fclose(f) != 0) { f = NULL; rc = ab_dev_fail(dev, ab_fail(AB_ERR_IO, "write to %s failed", o.out_path)); goto fail; }
    f = NULL;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "result", nbad ? "errors" : "ok");
    cJSON_AddNumberToObject(res, "start", lba);
    cJSON_AddNumberToObject(res, "count", count);
    cJSON_AddNumberToObject(res, "ok_sectors", (double)ok_sectors);
    cJSON_AddItemToObject(res, "bad_lbas", bad);
    if (cur < end) cJSON_AddNumberToObject(res, "next_lba", cur); else cJSON_AddNullToObject(res, "next_lba");
    cJSON_AddNumberToObject(res, "seconds", ab_round(seconds_since(t0), 1));
    if (nchunks > 20) {                     // long reads: only the chunks that failed
        cJSON *keep = cJSON_CreateArray(), *c;
        while ((c = cJSON_DetachItemFromArray(chunks, 0)) != NULL) {
            if (strcmp(ab_jstr(c, "result", ""), "ok") != 0) cJSON_AddItemToArray(keep, c);
            else cJSON_Delete(c);
        }
        cJSON_Delete(chunks);
        chunks = keep;
    }
    cJSON_AddItemToObject(res, "chunks", chunks);
    if (o.keep_data && o.data && o.data_len) { *o.data = data; *o.data_len = dlen; data = NULL; }
    free(data);
    *out = res;
    return AB_OK;
fail:
    if (f) fclose(f);
    cJSON_Delete(chunks);
    cJSON_Delete(bad);
    free(data);
    return rc;
}

int ab_scan(ab_dev *dev, uint32_t lba, uint32_t count, uint32_t timeout_ms, unsigned max_errors,
            double time_budget_s, ab_progress_cb progress, void *ctx, cJSON **out) {
    *out = NULL;
    if ((uint64_t)lba + count > 0xFFFFFFFFull) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "range beyond LBA 2^32"));
    uint64_t t0 = ab_now_ms();
    uint32_t end = lba + count, cur = lba;
    unsigned requests = 0, nbad = 0;
    cJSON *bad = cJSON_CreateArray(), *badl = cJSON_CreateArray();
    const char *stop_result = NULL;
    uint8_t stop_status = 0;
    char b[96];
    while (cur < end) {
        if (time_budget_s > 0 && seconds_since(t0) > time_budget_s) break;
        // One firmware VERIFY request may run ~10 s; with a budget keep each
        // request small (~2 s at old-drive speeds) so the budget is honoured.
        uint32_t n = end - cur;
        if (time_budget_s > 0 && n > 8192) n = 8192;
        ab_ataresult a;
        ab_reply r;
        cJSON *jerr = NULL;
        int e = ab_verify(dev, cur, n, timeout_ms, &a, &r, &jerr);
        ab_reply_free(&r);
        if (e != AB_OK && e != AB_ERR_API) { cJSON_Delete(bad); cJSON_Delete(badl); return e; }
        if (jerr) { cJSON_Delete(bad); cJSON_Delete(badl); *out = jerr; return AB_ERR_API; }
        requests++;
        if (a.result == R_OK && a.done == 0) break;   // no progress, no error: don't spin
        cur += a.done < n ? a.done : n;
        if (a.ide_result == APIIDE_NOT_READY || a.ide_result == APIIDE_BUS_FAULT) {
            // The drive isn't answering at all: not a bad sector, and skipping
            // ahead would only spin.
            stop_result = api_ide_result_name(a.ide_result);
            stop_status = a.status;
            break;
        }
        if (a.result != R_OK) {
            cJSON *x = cJSON_CreateObject();
            cJSON_AddNumberToObject(x, "lba", cur);
            cJSON_AddNumberToObject(x, "status", a.status);
            cJSON_AddNumberToObject(x, "error", a.error);
            cJSON_AddStringToObject(x, "error_bits", ab_error_bits(a.error, b));
            cJSON_AddStringToObject(x, "ide_result", api_ide_result_name(a.ide_result));
            cJSON_AddItemToArray(bad, x);
            cJSON_AddItemToArray(badl, cJSON_CreateNumber(cur));
            nbad++;
            if (progress) {
                char line[160];
                snprintf(line, sizeof(line), "bad LBA %u  st %02X err %02X[%s] %s", cur, a.status, a.error, b,
                         api_ide_result_name(a.ide_result));
                progress(ctx, line);
            }
            cur++;
            if (max_errors && nbad >= max_errors) break;
        }
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "result", stop_result ? stop_result : nbad ? "errors" : "ok");
    cJSON_AddNumberToObject(res, "start", lba);
    cJSON_AddNumberToObject(res, "count", count);
    cJSON_AddNumberToObject(res, "scanned", cur - lba);
    if (stop_result || cur < end) cJSON_AddNumberToObject(res, "next_lba", cur);
    else cJSON_AddNullToObject(res, "next_lba");
    cJSON_AddBoolToObject(res, "complete", !stop_result && cur >= end);
    cJSON_AddItemToObject(res, "bad", bad);
    cJSON_AddItemToObject(res, "bad_lbas", badl);
    if (stop_result) cJSON_AddNumberToObject(res, "status", stop_status);
    cJSON_AddNumberToObject(res, "seconds", ab_round(seconds_since(t0), 1));
    cJSON_AddNumberToObject(res, "requests", requests);
    *out = res;
    return AB_OK;
}

static void hex(const uint8_t *p, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    out[2 * n] = '\0';
}

int ab_read_long_vote(ab_dev *dev, uint32_t lba, unsigned votes, unsigned ecc_len, bool no_retry,
                      uint32_t timeout_ms, uint8_t *payload, size_t *payload_len, cJSON **info) {
    enum { MAXLEN = 512 + 255, MAXVOTES = 64 };
    *payload_len = 0;
    *info = NULL;
    if (votes < 1) votes = 1;
    if (votes > MAXVOTES) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "at most %d votes", MAXVOTES));
    static uint8_t reads[MAXVOTES][MAXLEN];       // ~49 KiB: not on the stack
    size_t lens[MAXVOTES];
    unsigned nreads = 0, attempts = 0;
    ab_ataresult last;
    memset(&last, 0, sizeof(last));
    cJSON *jlast = NULL;
    for (unsigned v = 0; v < votes; v++) {
        ab_ataresult a;
        ab_reply r;
        cJSON *jerr = NULL;
        int e = ab_read_long(dev, lba, ecc_len, no_retry, timeout_ms, &a, &r, &jerr);
        if (e != AB_OK && e != AB_ERR_API) { ab_reply_free(&r); cJSON_Delete(jlast); return e; }
        if (jerr) { ab_reply_free(&r); cJSON_Delete(jlast); *info = jerr; return AB_ERR_API; }
        attempts++;
        last = a;
        cJSON_Delete(jlast);
        jlast = ab_ataresult_json(&a);
        size_t n = a.data_len > MAXLEN ? MAXLEN : a.data_len;
        if (n) { memcpy(reads[nreads], a.data, n); lens[nreads] = n; nreads++; }
        ab_reply_free(&r);
        if (!n) break;
    }
    (void)attempts;
    *info = jlast;
    if (!nreads) return last.result == R_OK ? AB_OK : AB_ERR_API;
    size_t n = lens[0];
    unsigned good = 0;
    for (unsigned i = 0; i < nreads; i++)                    // vote only like with like
        if (lens[i] == n) { if (good != i) memcpy(reads[good], reads[i], n); good++; }
    unsigned unstable = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned counts[256] = {0};
        bool differ = false;
        for (unsigned k = 0; k < good; k++) {
            counts[reads[k][i]]++;
            if (reads[k][i] != reads[0][i]) differ = true;
        }
        // most common byte; ties go to the value seen first (like Python's max over a set
        // is arbitrary there; first-seen is deterministic)
        uint8_t best = reads[0][i];
        for (unsigned k = 0; k < good; k++)
            if (counts[reads[k][i]] > counts[best]) best = reads[k][i];
        payload[i] = best;
        unstable += differ;
    }
    *payload_len = n;
    cJSON *vote = cJSON_AddObjectToObject(*info, "vote");
    cJSON_AddNumberToObject(vote, "reads", good);
    cJSON_AddNumberToObject(vote, "unstable_bytes", unstable);
    char ecc[2 * 255 + 1];
    hex(payload + 512, n > 512 ? n - 512 : 0, ecc);
    cJSON_DeleteItemFromObjectCaseSensitive(*info, "ecc_hex");
    cJSON_AddStringToObject(*info, "ecc_hex", ecc);
    return AB_OK;
}
