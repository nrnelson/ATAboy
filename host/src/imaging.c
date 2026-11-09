// Imaging with a ddrescue mapfile, and targeted recovery of the mapfile's
// bad sectors (no-retry pass, drive retries, READ LONG vote).

#include "internal.h"
#include "api_proto.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SAVE_EVERY_S 10           // map checkpoint while imaging (as the Python tool)
#define API_MAX_SECTORS    7            // sectors per API READ while imaging
#define LBA28_LIMIT        (UINT64_C(1) << 28)

static volatile sig_atomic_t g_interrupted;

void ab_interrupt(void) { g_interrupted = 1; }
bool ab_interrupted(void) { return g_interrupted != 0; }
void ab_interrupt_reset(void) { g_interrupted = 0; }

static double secs_since(uint64_t t0_us) { return (double)(ab_now_us() - t0_us) / 1e6; }

static void say(ab_progress_cb cb, void *ctx, const char *fmt, ...) AB_PRINTF(3, 4);
static void say(ab_progress_cb cb, void *ctx, const char *fmt, ...) {
    if (!cb) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cb(ctx, line);
}

static int write_at(FILE *f, const char *path, uint64_t off, const uint8_t *data, size_t n) {
    if (off > (uint64_t)INT64_MAX || ab_fseek(f, (long long)off, SEEK_SET) != 0 || fwrite(data, 1, n, f) != n)
        return ab_fail(AB_ERR_IO, "write to %s failed: %s", path, strerror(errno));
    return AB_OK;
}

static char *concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (s) { memcpy(s, a, la); memcpy(s + la, b, lb + 1); }
    return s;
}

// ---------------------------------------------------------------------------
//  image
// ---------------------------------------------------------------------------

typedef struct {
    ab_dev  *dev;
    ab_blk  *blk;
    cJSON  **api_json;          // a JSON refusal from the device ends the run
} reader;

// n sectors at lba into dst.  AB_OK; AB_ERR_IO if any of them failed (a bad
// sector); anything else ends the run.
static int read_into(reader *rd, uint8_t *dst, uint64_t lba, unsigned n) {
    if (rd->blk) {
        int rc = ab_blk_read(rd->blk, lba * 512, dst, (size_t)n * 512);
        if (rc == AB_ERR_NODEV) {
            char m[AB_MSG_LEN];
            snprintf(m, sizeof(m), "%s", ab_last_error());
            return ab_fail(AB_ERR_TRANSPORT, "device went away during imaging (%s); map saved, rerun to resume", m);
        }
        return rc;
    }
    for (unsigned off = 0; off < n; off += API_MAX_SECTORS) {
        unsigned k = n - off < API_MAX_SECTORS ? n - off : API_MAX_SECTORS;
        ab_ataresult a;
        ab_reply r;
        cJSON *jerr = NULL;
        memset(&r, 0, offsetof(ab_reply, raw));
        int e = ab_read(rd->dev, (uint32_t)(lba + off), k, false, false, 0, &a, &r, &jerr);
        if (jerr) { ab_reply_free(&r); *rd->api_json = jerr; return AB_ERR_API; }
        if (e != AB_OK && e != AB_ERR_API) { ab_reply_free(&r); return e; }
        bool good = a.result == R_OK && a.data_len >= (size_t)k * 512;
        if (good) memcpy(dst + (size_t)off * 512, a.data, (size_t)k * 512);
        ab_reply_free(&r);
        if (!good) return ab_fail(AB_ERR_IO, "API read failed at LBA %llu", (unsigned long long)(lba + off));
    }
    return AB_OK;
}

static cJSON *local_reply(const char *result, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", result);
    cJSON_AddStringToObject(o, "message", message);
    return o;
}

int ab_image(ab_dev *dev, const char *out_path, const ab_image_opts *o_in, cJSON **out) {
    ab_image_opts o = {0};
    if (o_in) o = *o_in;
    *out = NULL;
    unsigned chunk = o.chunk_sectors ? o.chunk_sectors : 128;
    if (chunk > 65536) chunk = 65536;
    cJSON *st = NULL;
    int rc = ab_status(dev, &st);
    if (rc != AB_OK) { *out = st; return rc; }
    const cJSON *geo = ab_jobj_get(st, "geometry");
    if (!ab_jbool(geo, "valid", false)) {
        cJSON_Delete(st);
        *out = local_reply("no_geometry", "detect the drive (or set its geometry) first");
        ab_fail(AB_ERR_API, "no_geometry: detect the drive (or set its geometry) first");
        return ab_dev_fail(dev, AB_ERR_API);
    }
    double dtotal = ab_jnum(geo, "sectors", 0);
    bool mounted = ab_jbool(st, "mounted", false);
    cJSON_Delete(st);
    if (dtotal < 0 || dtotal > (double)(UINT64_C(1) << 53))
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "implausible geometry: %.0f sectors", dtotal));
    uint64_t total = (uint64_t)dtotal;
    uint64_t end = total;
    if (o.has_count && o.start <= total && o.count < total - o.start) end = o.start + o.count;

    reader rd = {dev, NULL, out};
    const ab_devinfo *di = ab_dev_info(dev);
    if (di && di->block[0] && mounted) {
        rc = ab_blk_open(di->block, &rd.blk);
        if (rc == AB_ERR_PERM || rc == AB_ERR_UNSUPPORTED) rd.blk = NULL;   // read through the API
        else if (rc != AB_OK) return ab_dev_fail(dev, rc);
    }

    char *mappath = o.mapfile ? concat(o.mapfile, "") : concat(out_path, ".map");
    uint8_t *buf = malloc((size_t)chunk * 512);
    uint64_t (*todo)[2] = NULL;
    size_t ntodo = 0;
    ab_mapfile *m = NULL;
    FILE *img = NULL;
    cJSON *bad = cJSON_CreateArray();
    if (!mappath || !buf || !bad) { rc = ab_fail(AB_ERR_NOMEM, "out of memory"); goto early; }
    rc = ab_file_exists(mappath) ? ab_mapfile_load(mappath, &m) : ab_mapfile_create(mappath, total * 512, &m);
    if (rc != AB_OK) goto early;
    {
        size_t nb;
        const ab_mapblock *b = ab_mapfile_blocks(m, &nb);
        todo = malloc((nb ? nb : 1) * sizeof(*todo));
        if (!todo) { rc = ab_fail(AB_ERR_NOMEM, "out of memory"); goto early; }
        for (size_t i = 0; i < nb; i++) {
            if (b[i].status != '?') continue;
            uint64_t a = b[i].pos / 512, z = (b[i].pos + b[i].size) / 512;
            if (a < o.start) a = o.start;
            if (z > end) z = end;
            if (a < z) { todo[ntodo][0] = a; todo[ntodo][1] = z; ntodo++; }
        }
    }
    if (!rd.blk)
        for (size_t i = 0; i < ntodo; i++)
            if (todo[i][1] > UINT64_C(0x100000000)) {
                rc = ab_fail(AB_ERR_UNSUPPORTED, "API READs address at most 2^32 sectors; image this drive "
                             "through the USB disk");
                goto early;
            }

    bool existed = ab_file_exists(out_path);
    img = fopen(out_path, existed ? "r+b" : "w+b");
    if (!img) { rc = ab_fail(AB_ERR_IO, "cannot open %s: %s", out_path, strerror(errno)); goto early; }
    ab_give_back(out_path);
    rc = ab_file_extend(img, out_path, total * 512);
    if (rc != AB_OK) goto early;

    uint64_t t0 = ab_now_us(), last_save = t0, done = 0;
    size_t nbad = 0;
    bool complete = true, interrupted = false;
    for (size_t t = 0; t < ntodo && rc == AB_OK; t++) {
        uint64_t lba = todo[t][0], b = todo[t][1];
        while (lba < b) {
            if (ab_interrupted()) { interrupted = true; complete = false; break; }
            if (o.time_budget_s > 0 && secs_since(t0) > o.time_budget_s) { complete = false; break; }
            unsigned n = b - lba < chunk ? (unsigned)(b - lba) : chunk;
            rc = read_into(&rd, buf, lba, n);
            if (rc == AB_OK) {
                if ((rc = write_at(img, out_path, lba * 512, buf, (size_t)n * 512)) != AB_OK) break;
                if ((rc = ab_mapfile_mark(m, lba, n, '+')) != AB_OK) break;
                done += n;
            } else if (rc == AB_ERR_IO) {
                rc = AB_OK;
                for (uint64_t s = lba; s < lba + n; s++) {         // isolate the bad sector(s)
                    if (ab_interrupted()) { interrupted = true; complete = false; break; }
                    rc = read_into(&rd, buf, s, 1);
                    if (rc == AB_OK) {
                        if ((rc = write_at(img, out_path, s * 512, buf, 512)) != AB_OK) break;
                        if ((rc = ab_mapfile_mark(m, s, 1, '+')) != AB_OK) break;
                        done++;
                    } else if (rc == AB_ERR_IO) {
                        rc = AB_OK;
                        if ((rc = ab_mapfile_mark(m, s, 1, '-')) != AB_OK) break;
                        cJSON_AddItemToArray(bad, cJSON_CreateNumber((double)s));
                        nbad++;
                        say(o.progress, o.ctx, "bad LBA %llu", (unsigned long long)s);
                    } else {
                        break;
                    }
                }
                if (rc != AB_OK || interrupted) break;
            } else {
                break;
            }
            lba += n;
            if (ab_now_us() - last_save > (uint64_t)IMAGE_SAVE_EVERY_S * 1000000u) {
                // image data first, then the map that claims it
                if ((rc = ab_file_sync(img, out_path)) != AB_OK) break;
                if ((rc = ab_mapfile_save(m)) != AB_OK) break;
                last_save = ab_now_us();
                double el = secs_since(t0);
                say(o.progress, o.ctx, "  %llu sectors imaged, %zu bad, %.0f kB/s", (unsigned long long)done, nbad,
                    (double)done * 0.512 / (el > 0.001 ? el : 0.001));
            }
        }
        if (!complete) break;
    }

    // Always keep what reached the image accounted for in the map.
    {
        char msg[AB_MSG_LEN];
        snprintf(msg, sizeof(msg), "%s", ab_last_error());
        int frc = img ? ab_file_sync(img, out_path) : AB_OK;
        if (img && fclose(img) != 0 && frc == AB_OK) frc = ab_fail(AB_ERR_IO, "cannot write %s", out_path);
        img = NULL;
        int src = ab_mapfile_save(m);
        if (rc == AB_OK) rc = frc != AB_OK ? frc : src;
        else ab_fail(rc, "%s", msg);              // the first error is the one to report
    }
    if (rd.blk) ab_blk_close(rd.blk);
    if (rc != AB_OK) {
        ab_mapfile_free(m);
        cJSON_Delete(bad);
        free(todo); free(buf); free(mappath);
        return ab_dev_fail(dev, rc);
    }
    double secs = secs_since(t0);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "result", interrupted ? "interrupted" : nbad ? "errors" : "ok");
    cJSON_AddStringToObject(res, "image", out_path);
    cJSON_AddStringToObject(res, "mapfile", mappath);
    cJSON_AddStringToObject(res, "via", rd.blk ? "usb_disk" : "api");
    cJSON_AddNumberToObject(res, "imaged_sectors", (double)done);
    cJSON_AddItemToObject(res, "bad_lbas", bad);
    cJSON_AddBoolToObject(res, "complete", complete);
    cJSON_AddItemToObject(res, "map", ab_mapfile_summary(m));
    cJSON_AddNumberToObject(res, "seconds", ab_round(secs, 1));
    if (secs > 0) cJSON_AddNumberToObject(res, "kB_per_s", ab_round((double)done * 0.512 / secs, 1));
    else cJSON_AddNullToObject(res, "kB_per_s");
    ab_mapfile_free(m);
    free(todo); free(buf); free(mappath);
    *out = res;
    return AB_OK;

early:                                          // nothing read yet
    if (img) fclose(img);
    if (rd.blk) ab_blk_close(rd.blk);
    ab_mapfile_free(m);
    cJSON_Delete(bad);
    free(todo); free(buf); free(mappath);
    return ab_dev_fail(dev, rc);
}

// ---------------------------------------------------------------------------
//  rescue
// ---------------------------------------------------------------------------

void ab_rescue_opts_init(ab_rescue_opts *o) {
    memset(o, 0, sizeof(*o));
    o->timeout_ms = 15000;
    o->votes = 5;
    o->readlong = true;
    o->no_retry_pass = true;
    o->save_interval_s = 30;
}

typedef struct {
    ab_dev *dev;
    const ab_rescue_opts *o;
    ab_mapfile *m;
    FILE *img;
    const char *image;
    uint64_t *targets;
    uint8_t *tried;             // per target: got a real answer from the drive, and it failed
    size_t *rem, nrem;          // indices into targets, in order
    uint64_t t0, last_save;
    unsigned saves;
    bool budget_hit, interrupted;
    cJSON *errors;
} rescue_st;

static bool budget_left(rescue_st *s) {
    if (ab_interrupted()) { s->interrupted = true; return false; }
    if (s->o->time_budget_s > 0 && secs_since(s->t0) >= s->o->time_budget_s) { s->budget_hit = true; return false; }
    return true;
}

static void api_error(rescue_st *s, uint64_t lba, const char *pass, const char *msg) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "lba", (double)lba);
    cJSON_AddStringToObject(e, "pass", pass);
    cJSON_AddStringToObject(e, "error", msg);
    cJSON_AddItemToArray(s->errors, e);
    say(s->o->progress, s->o->ctx, "LBA %llu: %s pass: %s", (unsigned long long)lba, pass, msg);
}

// Data first, then the map that claims it.
static int checkpoint(rescue_st *s, bool force) {
    if (!force && (double)(ab_now_us() - s->last_save) / 1e6 < s->o->save_interval_s) return AB_OK;
    int rc = ab_file_sync(s->img, s->image);
    if (rc == AB_OK) rc = ab_mapfile_save(s->m);
    if (rc != AB_OK) return rc;
    s->last_save = ab_now_us();
    s->saves++;
    return AB_OK;
}

static int read_pass(rescue_st *s, const char *pass, cJSON *recovered, bool no_retry, uint32_t timeout_ms) {
    size_t k = 0, i = 0;
    int rc = AB_OK;
    for (; i < s->nrem; i++) {
        size_t idx = s->rem[i];
        uint64_t lba = s->targets[idx];
        if (!budget_left(s)) break;
        if (lba > 0xFFFFFFFFu) {
            api_error(s, lba, pass, "LBA beyond the API's 32-bit range");
            s->rem[k++] = idx;
            continue;
        }
        ab_ataresult a;
        ab_reply r;
        cJSON *jerr = NULL;
        memset(&r, 0, offsetof(ab_reply, raw));
        int e = ab_read(s->dev, (uint32_t)lba, 1, no_retry, false, timeout_ms, &a, &r, &jerr);
        if (jerr) {
            char m[AB_MSG_LEN];
            snprintf(m, sizeof(m), "%s", ab_errmsg(s->dev));
            cJSON_Delete(jerr);
            ab_reply_free(&r);
            api_error(s, lba, pass, m);
            s->rem[k++] = idx;
            continue;
        }
        if (e != AB_OK && e != AB_ERR_API) { ab_reply_free(&r); rc = e; break; }
        if (a.result == R_OK && a.data_len >= 512) {
            rc = write_at(s->img, s->image, lba * 512, a.data, 512);
            double ms = a.elapsed_us / 1000.0;
            ab_reply_free(&r);
            if (rc == AB_OK) rc = ab_mapfile_mark(s->m, lba, 1, '+');
            if (rc != AB_OK) { s->rem[k++] = idx; i++; break; }
            cJSON_AddItemToArray(recovered, cJSON_CreateNumber((double)lba));
            s->tried[idx] = 0;
            say(s->o->progress, s->o->ctx, "LBA %llu recovered (%s pass, %.0f ms)", (unsigned long long)lba, pass, ms);
            if ((rc = checkpoint(s, false)) != AB_OK) { i++; break; }
        } else {
            ab_reply_free(&r);
            s->tried[idx] = 1;
            s->rem[k++] = idx;
        }
    }
    // Not reached (budget, ^C, error): still to do.
    for (; i < s->nrem; i++) s->rem[k++] = s->rem[i];
    s->nrem = k;
    return rc;
}

static int cmp_items(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    return strcmp(x->string ? x->string : "", y->string ? y->string : "");
}

// json.dump(sort_keys=True): every object's members in code-point order.
static int sort_keys(cJSON *j) {
    for (cJSON *c = j ? j->child : NULL; c; c = c->next)
        if (sort_keys(c) != AB_OK) return AB_ERR_NOMEM;
    if (!cJSON_IsObject(j) || !j->child) return AB_OK;
    size_t n = 0;
    for (cJSON *c = j->child; c; c = c->next) n++;
    cJSON **v = malloc(n * sizeof(*v));
    if (!v) return AB_ERR_NOMEM;
    n = 0;
    for (cJSON *c = j->child; c; c = c->next) v[n++] = c;
    qsort(v, n, sizeof(*v), cmp_items);
    for (size_t i = 0; i < n; i++) {
        v[i]->prev = i ? v[i - 1] : v[n - 1];      // cJSON: child->prev is the last item
        v[i]->next = i + 1 < n ? v[i + 1] : NULL;
    }
    j->child = v[0];
    free(v);
    return AB_OK;
}

static int load_json_file(const char *path, cJSON **out) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return ab_fail(AB_ERR_IO, "cannot open %s: %s", path, strerror(errno));
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= ((size_t)256 << 20)) { free(buf); fclose(f); return ab_fail(AB_ERR_IO, "%s is too large", path); }
            size_t nc = cap ? cap * 2 : 65536;
            char *nb = realloc(buf, nc);
            if (!nb) { free(buf); fclose(f); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
            buf = nb;
            cap = nc;
        }
        size_t k = fread(buf + len, 1, cap - len, f);
        len += k;
        if (!k) break;
    }
    fclose(f);
    cJSON *j = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return ab_fail(AB_ERR_IO, "%s: not a JSON object", path); }
    *out = j;
    return AB_OK;
}

static int write_sidecar(const char *path, cJSON *side) {
    if (sort_keys(side) != AB_OK) return ab_fail(AB_ERR_NOMEM, "out of memory");
    char *text = ab_json_print(side, 1);
    char *tmp = concat(path, ".tmp");
    if (!text || !tmp) { free(text); free(tmp); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    int rc = AB_OK;
    FILE *f = fopen(tmp, "wb");
    if (!f) rc = ab_fail(AB_ERR_IO, "cannot write %s: %s", tmp, strerror(errno));
    else {
        size_t n = strlen(text);
        if (fwrite(text, 1, n, f) != n) rc = ab_fail(AB_ERR_IO, "cannot write %s: %s", tmp, strerror(errno));
        if (rc == AB_OK) rc = ab_file_sync(f, tmp);
        if (fclose(f) != 0 && rc == AB_OK) rc = ab_fail(AB_ERR_IO, "cannot write %s", tmp);
        if (rc == AB_OK) rc = ab_file_replace(tmp, path);
        if (rc != AB_OK) remove(tmp);
        else ab_give_back(path);
    }
    free(text);
    free(tmp);
    return rc;
}

int ab_rescue(ab_dev *dev, const char *image, const char *mapfile, const ab_rescue_opts *o_in, cJSON **out) {
    ab_rescue_opts o;
    if (o_in) o = *o_in; else ab_rescue_opts_init(&o);
    *out = NULL;
    if (o.votes < 1) o.votes = 1;
    if (o.votes > 64) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "at most 64 READ LONG votes"));
    rescue_st s;
    memset(&s, 0, sizeof(s));
    s.dev = dev;
    s.o = &o;
    s.image = image;
    int rc = ab_mapfile_load(mapfile, &s.m);
    if (rc != AB_OK) return ab_dev_fail(dev, rc);
    size_t nt = 0;
    rc = ab_mapfile_bad_sectors(s.m, o.max_sectors, o.include_untried, &s.targets, &nt);
    if (rc != AB_OK) { ab_mapfile_free(s.m); return ab_dev_fail(dev, rc); }
    s.t0 = s.last_save = ab_now_us();
    s.tried = calloc(nt ? nt : 1, 1);
    s.rem = malloc((nt ? nt : 1) * sizeof(size_t));
    char *side_path = concat(image, ".readlong.json");
    cJSON *rec_nr = cJSON_CreateArray(), *rec_r = cJSON_CreateArray(), *rl = cJSON_CreateArray(),
          *rl_skip = cJSON_CreateArray(), *side = NULL;
    s.errors = cJSON_CreateArray();
    bool side_dirty = false, side_key = false;
    if (!s.tried || !s.rem || !side_path) { rc = ab_fail(AB_ERR_NOMEM, "out of memory"); goto finally; }
    for (size_t i = 0; i < nt; i++) s.rem[i] = i;
    s.nrem = nt;

    s.img = fopen(image, "r+b");
    if (!s.img) { rc = ab_fail(AB_ERR_IO, "cannot open %s: %s", image, strerror(errno)); goto finally; }
    // Pass 1: no drive retries, single sectors (fast fail, gentle)
    if (o.no_retry_pass) {
        rc = read_pass(&s, "no-retry", rec_nr, true, o.timeout_ms < 5000 ? o.timeout_ms : 5000);
        if (rc != AB_OK || s.interrupted) goto finally;
        if ((rc = checkpoint(&s, true)) != AB_OK) goto finally;
    }
    // Pass 2: drive retries, long timeout
    rc = read_pass(&s, "retry", rec_r, false, o.timeout_ms);
    if (rc != AB_OK || s.interrupted) goto finally;
    if ((rc = checkpoint(&s, true)) != AB_OK) goto finally;
    // Pass 3: READ LONG majority vote (data without ECC correction)
    if (o.readlong && s.nrem) {
        if (ab_file_exists(side_path)) {
            if ((rc = load_json_file(side_path, &side)) != AB_OK) goto finally;
        } else {
            side = cJSON_CreateObject();
        }
        for (size_t i = 0; i < s.nrem; i++) {          // every one stays bad in the map
            size_t idx = s.rem[i];
            uint64_t lba = s.targets[idx];
            if (!budget_left(&s)) break;
            if (lba >= LBA28_LIMIT) {
                cJSON *x = cJSON_CreateObject();
                cJSON_AddNumberToObject(x, "lba", (double)lba);
                cJSON_AddStringToObject(x, "reason", "READ LONG has no 48-bit form (LBA >= 2^28)");
                cJSON_AddItemToArray(rl_skip, x);
                continue;
            }
            uint8_t payload[512 + 255];
            size_t plen = 0;
            cJSON *info = NULL;
            int e = ab_read_long_vote(dev, (uint32_t)lba, o.votes, 0, false, o.timeout_ms, payload, &plen, &info);
            if (e == AB_ERR_API && info && !ab_jobj_get(info, "ide_result")) {   // the device refused
                char m[AB_MSG_LEN];
                snprintf(m, sizeof(m), "%s", ab_errmsg(dev));
                cJSON_Delete(info);
                api_error(&s, lba, "readlong", m);
                continue;
            }
            if (e != AB_OK && e != AB_ERR_API) { cJSON_Delete(info); rc = e; goto finally; }
            s.tried[idx] = 1;
            if (!plen) { cJSON_Delete(info); continue; }
            rc = write_at(s.img, image, lba * 512, payload, 512);
            if (rc != AB_OK) { cJSON_Delete(info); goto finally; }
            const cJSON *vote = ab_jobj_get(info, "vote");
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "lba", (double)lba);
            cJSON_AddStringToObject(entry, "ecc_hex", ab_jstr(info, "ecc_hex", ""));
            cJSON_AddNumberToObject(entry, "votes", ab_jnum(vote, "reads", 0));
            cJSON_AddNumberToObject(entry, "unstable_bytes", ab_jnum(vote, "unstable_bytes", 0));
            cJSON_AddNumberToObject(entry, "status", ab_jnum(info, "status", 0));
            cJSON_AddStringToObject(entry, "error_bits", ab_jstr(info, "error_bits", ""));
            cJSON_Delete(info);
            char key[24];
            snprintf(key, sizeof(key), "%llu", (unsigned long long)lba);
            cJSON_DeleteItemFromObjectCaseSensitive(side, key);
            cJSON_AddItemToObject(side, key, cJSON_Duplicate(entry, true));
            side_dirty = true;
            cJSON_AddItemToArray(rl, entry);
            say(o.progress, o.ctx, "LBA %llu: READ LONG data written (%.0f unstable bytes over %.0f reads)",
                (unsigned long long)lba, ab_jnum(entry, "unstable_bytes", 0), ab_jnum(entry, "votes", 0));
        }
        side_key = true;
    }

finally:
    {
        char msg[AB_MSG_LEN];
        snprintf(msg, sizeof(msg), "%s", ab_last_error());
        int frc = AB_OK;
        if (s.img) {
            if (fflush(s.img) != 0) frc = ab_fail(AB_ERR_IO, "cannot write %s", image);
            if (fclose(s.img) != 0 && frc == AB_OK) frc = ab_fail(AB_ERR_IO, "cannot write %s", image);
            s.img = NULL;
        }
        // Tried, failed, and never tried before: now known bad.
        for (size_t i = 0; s.rem && i < s.nrem; i++) {
            size_t idx = s.rem[i];
            if (s.tried[idx] && ab_mapfile_status_at(s.m, s.targets[idx]) == '?') {
                int mrc = ab_mapfile_mark(s.m, s.targets[idx], 1, '-');
                if (mrc != AB_OK && frc == AB_OK) frc = mrc;
            }
        }
        int src = ab_mapfile_save(s.m);
        if (src != AB_OK && frc == AB_OK) frc = src;
        if (side && (side_dirty || (o.readlong && side_key))) {
            int wrc = write_sidecar(side_path, side);
            if (wrc != AB_OK && frc == AB_OK) frc = wrc;
        }
        if (rc == AB_OK) rc = frc;
        else ab_fail(rc, "%s", msg);              // report the first error
    }
    if (rc != AB_OK) {
        cJSON_Delete(rec_nr); cJSON_Delete(rec_r); cJSON_Delete(rl); cJSON_Delete(rl_skip);
        cJSON_Delete(s.errors); cJSON_Delete(side);
        free(s.targets); free(s.tried); free(s.rem); free(side_path);
        ab_mapfile_free(s.m);
        return ab_dev_fail(dev, rc);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "result", s.interrupted ? "interrupted" : s.nrem ? "partial" : "ok");
    cJSON_AddStringToObject(res, "image", image);
    cJSON_AddStringToObject(res, "mapfile", mapfile);
    cJSON_AddNumberToObject(res, "candidates", (double)nt);
    cJSON_AddItemToObject(res, "recovered_no_retry", rec_nr);
    cJSON_AddItemToObject(res, "recovered_retry", rec_r);
    cJSON_AddItemToObject(res, "readlong", rl);
    cJSON *still = cJSON_AddArrayToObject(res, "still_bad");
    for (size_t i = 0; i < s.nrem; i++) cJSON_AddItemToArray(still, cJSON_CreateNumber((double)s.targets[s.rem[i]]));
    cJSON_AddItemToObject(res, "readlong_skipped", rl_skip);
    cJSON_AddItemToObject(res, "errors", s.errors);
    cJSON_AddBoolToObject(res, "complete", !(s.budget_hit || s.interrupted));
    if (side_key) cJSON_AddStringToObject(res, "readlong_sidecar", side_path);
    cJSON_AddNumberToObject(res, "map_saves", s.saves + 1);
    cJSON_AddNumberToObject(res, "recovered", cJSON_GetArraySize(rec_nr) + cJSON_GetArraySize(rec_r));
    cJSON_AddNumberToObject(res, "seconds", ab_round(secs_since(s.t0), 1));
    cJSON_Delete(side);
    free(s.targets); free(s.tried); free(s.rem); free(side_path);
    ab_mapfile_free(s.m);
    *out = res;
    return AB_OK;
}
