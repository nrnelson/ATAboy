// ataboy: command-line client for the ATAboy automation API.
//
// Same commands, options, JSON output and exit codes as the original Python
// tool: 0 ok, 1 the device reported a failure, 2 usage, 3 transport / no
// device, 4 permission.  No root needed on the API serial port.

#include "cli.h"
#include "api_proto.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#endif

#define VERSION ATABOY_LIB_VERSION

// ---------------------------------------------------------------------------
//  Command table and argument parsing
// ---------------------------------------------------------------------------

typedef struct { const char *name; bool arg; const char *help; } opt_t;
typedef struct ctx ctx;
typedef struct {
    const char *name;
    int (*fn)(ctx *c);
    bool needs_dev;
    int min_pos, max_pos;
    const char *pos_help;
    const char *help;
    opt_t opts[16];
} cmd_t;

#define MAXPOS 64
struct ctx {
    bool json, compact, verbose;
    const char *dev, *serial, *transport;
    const cmd_t *cmd;
    const char *pos[MAXPOS];
    int npos;
    const char *val[16];
    bool set[16];
    ab_dev *d;
};

static int opt_index(const ctx *c, const char *name) {
    for (int i = 0; i < 16 && c->cmd->opts[i].name; i++)
        if (!strcmp(c->cmd->opts[i].name, name)) return i;
    return -1;
}
static bool flag(const ctx *c, const char *name) { int i = opt_index(c, name); return i >= 0 && c->set[i]; }
static const char *opt(const ctx *c, const char *name) { int i = opt_index(c, name); return i >= 0 && c->set[i] ? c->val[i] : NULL; }

#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 1, 2), noreturn))
#elif defined(__GNUC__)
__attribute__((format(printf, 1, 2), noreturn))
#endif
static void usage_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ataboy: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n(try: ataboy --help)\n");
    exit(EXIT_USAGE);
}

// Python int(s, 0): decimal, 0x.., 0o.., 0b..
static long long parse_int(const char *s, const char *what) {
    char *end;
    int base = 10;
    const char *p = s;
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    errno = 0;
    long long v = strtoll(p, &end, base);
    if (!*p || *end || errno) usage_error("bad number for %s", what);
    return neg ? -v : v;
}
static long long iopt(const ctx *c, const char *name, long long dflt) {
    const char *v = opt(c, name);
    return v ? parse_int(v, name) : dflt;
}
static double fopt(const ctx *c, const char *name, double dflt) {
    const char *v = opt(c, name);
    if (!v) return dflt;
    char *end;
    double d = strtod(v, &end);
    if (!*v || *end) usage_error("bad number for --%s", name);
    return d;
}
static uint32_t pos_u32(const ctx *c, int i, const char *what) {
    long long v = parse_int(c->pos[i], what);
    if (v < 0 || v > 0xFFFFFFFFll) usage_error("%s out of range", what);
    return (uint32_t)v;
}

static void sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

// ---------------------------------------------------------------------------
//  Output
// ---------------------------------------------------------------------------

typedef void (*human_fn)(const cJSON *j, ctx *c);

static void print_json(const ctx *c, const cJSON *j) {
    char *s = ab_json_print(j, c->compact ? 0 : 2);
    if (s) { puts(s); free(s); }
}

static void emit(ctx *c, const cJSON *j, human_fn h) {
    if (c->json || !h) print_json(c, j);
    else h(j, c);
    fflush(stdout);
}

static int exit_for(int err) {
    switch (err) {
    case AB_OK: return EXIT_OK;
    case AB_ERR_API: case AB_ERR_IO: return EXIT_DEVICE;
    case AB_ERR_ARG: return EXIT_USAGE;
    case AB_ERR_PERM: return EXIT_PERM;
    default: return EXIT_TRANSPORT;
    }
}

// Report a failed library call; reply: the device's JSON, if it sent one.
static int fail(ctx *c, int err, cJSON *reply) {
    const char *msg = c->d ? ab_errmsg(c->d) : ab_last_error();
    if (err == AB_ERR_API && reply) {
        if (c->json) print_json(c, reply);
        else fprintf(stderr, "error: %s\n", msg);
        cJSON_Delete(reply);
        return EXIT_DEVICE;
    }
    cJSON_Delete(reply);
    char m[700];
    snprintf(m, sizeof(m), "%s", msg);
    if (err == AB_ERR_CHECK && c->d) {
        uint8_t k, asc, q;
        ab_sense(c->d, &k, &asc, &q);
        const ab_devinfo *di = ab_dev_info(c->d);
        if (k == 5 && asc == 0x24 && di && di->rev[0] && strncmp(di->rev, "api", 3) != 0)
            snprintf(m, sizeof(m), "firmware revision '%s' does not implement the API; flash the API firmware", di->rev);
    }
    const char *name = err == AB_ERR_PERM ? "permission_denied" : err == AB_ERR_API ? "error"
                     : err == AB_ERR_ARG ? "bad_argument" : err == AB_ERR_IO ? "io_error" : "transport_error";
    if (c->json) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "result", name);
        cJSON_AddStringToObject(o, "message", m);
        print_json(c, o);
        cJSON_Delete(o);
    } else if (err == AB_ERR_PERM) {
        fprintf(stderr, "error: %s\n", m);
    } else {
        fprintf(stderr, "error: %s\n", m);
    }
    return exit_for(err);
}

// Commands that return one JSON object from the device: emit it, map the result.
static int done_json(ctx *c, int rc, cJSON *j, human_fn h) {
    if (rc != AB_OK && !(rc == AB_ERR_API && j)) return fail(c, rc, j);
    emit(c, j, h);
    cJSON_Delete(j);
    return rc == AB_OK ? EXIT_OK : EXIT_DEVICE;
}

// ---------------------------------------------------------------------------
//  Human renderers bound to ctx
// ---------------------------------------------------------------------------

static void h_status(const cJSON *j, ctx *c) { (void)c; human_status(j); }
static void h_kv(const cJSON *j, ctx *c) { (void)c; human_keyvals(j, "words"); }
static void h_message(const cJSON *j, ctx *c) { (void)c; puts(ab_jstr(j, "message", ab_jstr(j, "result", ""))); }

static void h_status_reply(const cJSON *j, ctx *c) {
    const char *m = ab_jstr(j, "message", NULL);
    printf("%s: %s%s%s\n", c->cmd->name, ab_jstr(j, "result", "?"), m ? " - " : "", m ? m : "");
    if (cJSON_IsObject(ab_jobj_get(j, "drive"))) human_status(j);
}

static void h_info(const cJSON *j, ctx *c) {
    (void)c;
    const cJSON *h = ab_jobj_get(j, "host");
    printf("%s (%s, proto %d) built %s  board %s %s  via %s", ab_jstr(j, "fw", "?"), ab_jstr(j, "git", "?"),
           (int)ab_jnum(j, "proto", 0), ab_jstr(j, "built", "?"), ab_jstr(j, "board", "?"), ab_jstr(j, "board_id", "?"),
           ab_jstr(h, "transport", "?"));
    const cJSON *b = ab_jobj_get(h, "boot_id");
    if (cJSON_IsNumber(b)) printf("  boot %08x state %u", (unsigned)b->valuedouble, (unsigned)ab_jnum(h, "state_counter", 0));
    printf("\n");
}

// ---------------------------------------------------------------------------
//  Commands
// ---------------------------------------------------------------------------

static int cmd_list(ctx *c) {
    ab_devinfo devs[16];
    int n = ab_find_devices(devs, 16);
    if (n > 16) n = 16;
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        const ab_devinfo *d = &devs[i];
#define S(k, v) if ((v)[0]) cJSON_AddStringToObject(o, k, v); else cJSON_AddNullToObject(o, k)
        S("block", d->block); S("sg", d->sg); S("tty", d->tty);
        cJSON_AddStringToObject(o, "serial", d->serial);
        cJSON_AddStringToObject(o, "rev", d->rev);
        cJSON_AddNumberToObject(o, "size_sectors", (double)d->size_sectors);
        cJSON_AddNumberToObject(o, "scsi_timeout_s", d->scsi_timeout_s);
        S("sysfs", d->sysfs); S("usb", d->usb);
#undef S
        cJSON_AddItemToArray(a, o);
    }
    if (c->json) print_json(c, a);
    else if (!n) puts("no ATAboy found");
    else
        for (int i = 0; i < n; i++)
            printf("%s  %s  %s  serial %s  rev %s  %llu sectors\n", devs[i].block[0] ? devs[i].block : "None",
                   devs[i].sg[0] ? devs[i].sg : "None", devs[i].tty[0] ? devs[i].tty : "-",
                   devs[i].serial[0] ? devs[i].serial : "-", devs[i].rev[0] ? devs[i].rev : "-",
                   (unsigned long long)devs[i].size_sectors);
    cJSON_Delete(a);
    return EXIT_OK;
}

static int cmd_info(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_info(c->d, &j);
    if (rc == AB_OK) cJSON_AddItemToObject(j, "host", ab_link_info(c->d));
    return done_json(c, rc, j, h_info);
}

static void add_host(ctx *c, cJSON *j) {
    cJSON *h = cJSON_CreateObject();
    const ab_devinfo *d = ab_dev_info(c->d);
#define S(k, v) if (d && (v)[0]) cJSON_AddStringToObject(h, k, v); else cJSON_AddNullToObject(h, k)
    S("block", d ? d->block : ""); S("sg", d ? d->sg : ""); S("tty", d ? d->tty : "");
#undef S
    cJSON *li = ab_link_info(c->d), *x;
    if (d && d->scsi_timeout_s) cJSON_AddNumberToObject(h, "scsi_timeout_s", d->scsi_timeout_s);
    else cJSON_AddNullToObject(h, "scsi_timeout_s");
    while ((x = li->child) != NULL) { cJSON_DetachItemViaPointer(li, x); cJSON_AddItemToObject(h, x->string, x); }
    cJSON_Delete(li);
    cJSON_AddItemToObject(j, "host", h);
}

static int cmd_status(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_status(c->d, &j);
    if (rc == AB_OK) add_host(c, j);
    return done_json(c, rc, j, h_status);
}

static void h_config(const cJSON *j, ctx *c) {
    (void)c;
    for (const cJSON *x = ab_jobj_get(j, "config")->child; x; x = x->next) {
        if (cJSON_IsBool(x)) printf("%-18s %s\n", x->string, cJSON_IsTrue(x) ? "True" : "False");
        else if (cJSON_IsString(x)) printf("%-18s %s\n", x->string, x->valuestring);
        else printf("%-18s %.0f\n", x->string, x->valuedouble);
    }
}

static int cmd_config(ctx *c) {
    cJSON *cfg = NULL;
    for (int i = 0; i < c->npos; i++) {
        const char *eq = strchr(c->pos[i], '=');
        if (!eq) {
            fprintf(stderr, "settings look like key=value; keys:");
            for (int k = 0; ab_config_keys[k]; k++) fprintf(stderr, "%s %s", k ? "," : "", ab_config_keys[k]);
            fprintf(stderr, "\n");
            return EXIT_USAGE;
        }
        char key[64];
        snprintf(key, sizeof(key), "%.*s", (int)(eq - c->pos[i]), c->pos[i]);
        for (char *p = key; *p; p++) if (*p == '-') *p = '_';
        if (!strcmp(key, "read_timeout")) strcpy(key, "read_timeout_ms");
        cJSON_Delete(cfg);
        cfg = NULL;
        int rc = ab_config_set(c->d, key, eq + 1, &cfg);
        if (rc != AB_OK) return fail(c, rc, cfg);
    }
    if (flag(c, "save")) {
        cJSON *j = NULL;
        int rc = ab_config_save(c->d, &j);
        cJSON_Delete(j);
        if (rc != AB_OK) { cJSON_Delete(cfg); return fail(c, rc, NULL); }
    }
    if (!cfg) {
        int rc = ab_config_get(c->d, &cfg);
        if (rc != AB_OK) return fail(c, rc, cfg);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", "ok");
    cJSON_AddItemToObject(o, "config", cfg);
    cJSON_AddBoolToObject(o, "saved", flag(c, "save"));
    const ab_devinfo *d = ab_dev_info(c->d);
    if (d && d->sysfs[0]) cJSON_AddNumberToObject(o, "kernel_scsi_timeout_s", d->scsi_timeout_s);
    emit(c, o, h_config);
    cJSON_Delete(o);
    return EXIT_OK;
}

static void parse_chs(const char *s, unsigned *cy, unsigned *h, unsigned *sp) {
    unsigned a, b, d;
    char x1, x2, extra;
    if (!s || sscanf(s, "%u%c%u%c%u%c", &a, &x1, &b, &x2, &d, &extra) != 5 || (x1 != '/' && x1 != ',') ||
        (x2 != '/' && x2 != ','))
        usage_error("CHS must look like 989/10/34");
    *cy = a; *h = b; *sp = d;
}

static int cmd_detect(ctx *c) {
    ab_detect_opts o = {0};
    o.device = opt(c, "device") ? opt(c, "device") : "auto";
    o.geometry = opt(c, "geometry") ? opt(c, "geometry") : "auto";
    if (opt(c, "chs")) parse_chs(opt(c, "chs"), &o.cyls, &o.heads, &o.spt);
    o.no_probe = flag(c, "no-probe");
    o.force = flag(c, "force");
    o.mount = flag(c, "mount");
    o.force_mount = flag(c, "force-mount");
    cJSON *j = NULL;
    int rc = ab_detect(c->d, &o, &j);
    return done_json(c, rc, j, h_status_reply);
}

static int cmd_geometry(ctx *c) {
    unsigned cy = 0, h = 0, s = 0;
    if (opt(c, "chs")) parse_chs(opt(c, "chs"), &cy, &h, &s);
    cJSON *j = NULL;
    int rc = ab_geometry(c->d, c->pos[0], cy, h, s, &j);
    return done_json(c, rc, j, h_status_reply);
}

static int cmd_mount(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_mount(c->d, flag(c, "force"), &j);
    return done_json(c, rc, j, h_status_reply);
}
static int cmd_unmount(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_unmount(c->d, &j);
    return done_json(c, rc, j, h_status_reply);
}
static int cmd_reset(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_reset(c->d, flag(c, "hard"), &j);
    return done_json(c, rc, j, h_status_reply);
}
static void h_park(const cJSON *j, ctx *c) { (void)c; human_park(j); }
static int cmd_park(ctx *c) {
    long long lz = iopt(c, "landing-zone", -1);
    if (opt(c, "landing-zone") && (lz < 0 || lz > 65535)) usage_error("landing-zone cylinder must be 0..65535");
    cJSON *j = NULL;
    int rc = ab_park(c->d, (int)lz, &j);
    return done_json(c, rc, j, h_park);
}

static int cmd_identify(ctx *c) {
    uint8_t raw[512];
    int rc = ab_identify(c->d, flag(c, "cached"), raw);
    if (rc != AB_OK) return fail(c, rc, NULL);
    if (opt(c, "raw")) {
        FILE *f = fopen(opt(c, "raw"), "wb");
        if (!f || fwrite(raw, 1, 512, f) != 512) { if (f) fclose(f); fprintf(stderr, "error: cannot write %s\n", opt(c, "raw")); return EXIT_DEVICE; }
        fclose(f);
    }
    cJSON *d = ab_decode_identify(raw);
    if (flag(c, "words")) {
        cJSON *w = cJSON_AddArrayToObject(d, "words");
        for (int i = 0; i < 256; i++) cJSON_AddItemToArray(w, cJSON_CreateNumber(raw[2 * i] | (raw[2 * i + 1] << 8)));
    }
    emit(c, d, h_kv);
    cJSON_Delete(d);
    return EXIT_OK;
}

static void h_taskfile(const cJSON *j, ctx *c) { (void)c; human_taskfile(j); }
static int cmd_taskfile(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_taskfile(c->d, &j);
    return done_json(c, rc, j, h_taskfile);
}

static uint8_t *g_data;            // read --hexdump
static size_t g_dlen;
static uint32_t g_lba;

static void h_read(const cJSON *s, ctx *c) {
    for (const cJSON *x = ab_jobj_get(s, "chunks")->child; x; x = x->next)
        if (strcmp(ab_jstr(x, "result", ""), "ok") || c->verbose) human_ata_result(x);
    const cJSON *bad = ab_jobj_get(s, "bad_lbas");
    int nb = cJSON_GetArraySize(bad);
    printf("read %.0f sectors from LBA %.0f: %.0f ok, %d bad", ab_jnum(s, "count", 0), ab_jnum(s, "start", 0),
           ab_jnum(s, "ok_sectors", 0), nb);
    if (nb) {
        printf(" [");
        int i = 0;
        for (const cJSON *x = bad->child; x && i < 20; x = x->next, i++) printf("%s%.0f", i ? ", " : "", x->valuedouble);
        printf("]");
    }
    printf("\n");
    if (flag(c, "hexdump")) hexdump(g_data, g_dlen, (uint64_t)g_lba * 512);
}

static char *to_hex(const uint8_t *p, size_t n) {
    char *s = malloc(2 * n + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", p[i]);
    s[2 * n] = '\0';
    return s;
}

static int cmd_read(ctx *c) {
    uint32_t lba = pos_u32(c, 0, "lba"), count = c->npos > 1 ? pos_u32(c, 1, "count") : 1;
    ab_read_range_opts o = {0};
    o.out_path = opt(c, "out");
    o.image = flag(c, "image");
    o.no_retry = flag(c, "no-retry");
    o.through_error = flag(c, "through-error");
    o.keep_going = flag(c, "keep-going");
    o.timeout_ms = (uint32_t)iopt(c, "timeout", 0);
    o.keep_data = flag(c, "hex") || flag(c, "hexdump");
    o.data = &g_data;
    o.data_len = &g_dlen;
    g_lba = lba;
    cJSON *res = NULL;
    int rc = ab_read_range(c->d, lba, count, &o, &res);
    if (rc != AB_OK) return fail(c, rc, res);
    if (flag(c, "hex")) {
        char *h = to_hex(g_data, g_dlen);
        cJSON_AddStringToObject(res, "data_hex", h ? h : "");
        free(h);
    }
    emit(c, res, h_read);
    int ex = strcmp(ab_jstr(res, "result", ""), "ok") ? EXIT_DEVICE : EXIT_OK;
    cJSON_Delete(res);
    free(g_data);
    return ex;
}

static void h_readlong(const cJSON *o, ctx *c) {
    (void)c;
    human_ata_result(o);
    const cJSON *v = ab_jobj_get(o, "vote");
    printf("ECC bytes: %s", ab_jstr(o, "ecc_hex", "-"));
    if (v) printf("  vote: %.0f reads, %.0f unstable bytes", ab_jnum(v, "reads", 0), ab_jnum(v, "unstable_bytes", 0));
    printf("\n");
}

static int cmd_readlong(ctx *c) {
    uint32_t lba = pos_u32(c, 0, "lba");
    uint8_t payload[512 + 255];
    size_t n = 0;
    cJSON *info = NULL;
    int rc = ab_read_long_vote(c->d, lba, (unsigned)iopt(c, "vote", 1), (unsigned)iopt(c, "ecc", 0),
                               flag(c, "no-retry"), (uint32_t)iopt(c, "timeout", 0), payload, &n, &info);
    if (rc != AB_OK && !(rc == AB_ERR_API && info && ab_jobj_get(info, "ide_result"))) return fail(c, rc, info);
    if (n) {
        if (opt(c, "out")) {
            size_t w = flag(c, "with-ecc") ? n : 512;
            FILE *f = fopen(opt(c, "out"), "wb");
            if (!f || fwrite(payload, 1, w, f) != w) { if (f) fclose(f); cJSON_Delete(info); fprintf(stderr, "error: cannot write %s\n", opt(c, "out")); return EXIT_DEVICE; }
            fclose(f);
            cJSON_AddStringToObject(info, "written", opt(c, "out"));
        }
        if (opt(c, "image")) {
            FILE *f = fopen(opt(c, "image"), "r+b");
            bool ok = f && fseek(f, 0, SEEK_SET) == 0;
#if defined(_WIN32)
            ok = ok && _fseeki64(f, (long long)lba * 512, SEEK_SET) == 0;
#else
            ok = ok && fseeko(f, (off_t)lba * 512, SEEK_SET) == 0;
#endif
            ok = ok && fwrite(payload, 1, 512, f) == 512;
            if (f) ok = fclose(f) == 0 && ok;
            if (!ok) { cJSON_Delete(info); fprintf(stderr, "error: cannot patch %s\n", opt(c, "image")); return EXIT_DEVICE; }
            cJSON_AddStringToObject(info, "patched_image", opt(c, "image"));
        }
    }
    emit(c, info, h_readlong);
    cJSON_Delete(info);
    return n ? EXIT_OK : EXIT_DEVICE;
}

static void progress_line(void *ctxp, const char *line) { (void)ctxp; printf("%s\n", line); fflush(stdout); }

static void h_verify(const cJSON *o, ctx *c) {
    (void)c;
    printf("verified %.0f sectors from LBA %.0f in %.1f s: %d bad", ab_jnum(o, "scanned", 0), ab_jnum(o, "start", 0),
           ab_jnum(o, "seconds", 0), cJSON_GetArraySize(ab_jobj_get(o, "bad")));
    if (!ab_jbool(o, "complete", true)) {
        const cJSON *n = ab_jobj_get(o, "next_lba");
        if (cJSON_IsNumber(n)) printf(" (stopped early; resume at LBA %.0f)", n->valuedouble);
        else printf(" (stopped early; resume at LBA None)");
    }
    printf("\n");
}

static int cmd_verify(ctx *c) {
    cJSON *out = NULL;
    int rc = ab_scan(c->d, pos_u32(c, 0, "lba"), pos_u32(c, 1, "count"), (uint32_t)iopt(c, "timeout", 0),
                     (unsigned)iopt(c, "max-errors", 0), fopt(c, "budget", 0), c->json ? NULL : progress_line, NULL, &out);
    if (rc != AB_OK) return fail(c, rc, out);
    emit(c, out, h_verify);
    int ex = cJSON_GetArraySize(ab_jobj_get(out, "bad")) ? EXIT_DEVICE : EXIT_OK;
    cJSON_Delete(out);
    return ex;
}

static void h_ata(const cJSON *j, ctx *c) { (void)c; human_ata_result(j); }
static int cmd_corrupt(ctx *c) {
    ab_ataresult a;
    ab_reply r;
    cJSON *jerr = NULL;
    int rc = ab_corrupt(c->d, pos_u32(c, 0, "lba"), (unsigned)iopt(c, "bits", 16), (unsigned)iopt(c, "ecc", 0), &a, &r, &jerr);
    if (jerr || (rc != AB_OK && rc != AB_ERR_API)) { ab_reply_free(&r); return fail(c, rc, jerr); }
    cJSON *d = ab_ataresult_json(&a);
    ab_reply_free(&r);
    emit(c, d, h_ata);
    cJSON_Delete(d);
    return rc == AB_OK ? EXIT_OK : EXIT_DEVICE;
}

static void h_log(const cJSON *j, ctx *c) {
    (void)c;
    for (const cJSON *x = ab_jobj_get(j, "entries")->child; x; x = x->next) human_log_entry(x, "");
    printf("%.0f failures logged, %.0f retained\n", ab_jnum(j, "total", 0), ab_jnum(j, "retained", 0));
}

static int cmd_log(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_log(c->d, &j);
    if (rc != AB_OK) return fail(c, rc, j);
    if (flag(c, "clear")) {
        cJSON *x = NULL;
        rc = ab_clear(c->d, true, false, &x);
        cJSON_Delete(x);
        if (rc != AB_OK) { cJSON_Delete(j); return fail(c, rc, NULL); }
        cJSON_AddBoolToObject(j, "cleared", true);
    }
    emit(c, j, h_log);
    cJSON_Delete(j);
    return EXIT_OK;
}

static int cmd_clear(ctx *c) {
    cJSON *j = NULL;
    bool so = flag(c, "stats-only");
    int rc = ab_clear(c->d, !so, flag(c, "stats") || so, &j);
    return done_json(c, rc, j, NULL);
}

static int cmd_arm(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_arm(c->d, &j);
    return done_json(c, rc, j, h_message);
}

static bool device_present(const char *serial, const char *tty) {
    ab_devinfo devs[16];
    int n = ab_find_devices(devs, 16);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        if (serial && *serial && !strcasecmp(devs[i].serial, serial)) return true;
        if ((!serial || !*serial) && tty && *tty && !strcmp(devs[i].tty, tty)) return true;
    }
    return false;
}

static int cmd_reboot(ctx *c) {
    cJSON *j = NULL;
    int rc = ab_reboot(c->d, flag(c, "bootsel") ? 1 : 0, &j);
    if (rc != AB_OK) return fail(c, rc, j);
    const ab_devinfo *d = ab_dev_info(c->d);
    char serial[64] = "", tty[256] = "";
    if (d) { snprintf(serial, sizeof(serial), "%s", d->serial); snprintf(tty, sizeof(tty), "%s", d->tty); }
    bootcache_save(c->d);
    ab_close(c->d);
    c->d = NULL;
    // The firmware reboots ~200 ms after replying; don't return while the old
    // instance is still there, or a following "wait" would talk to it.
    int waited = 0;
    while ((serial[0] || tty[0]) && device_present(serial, tty) && waited < 50) {
        sleep_ms(100);
        waited++;
    }
    cJSON_AddNumberToObject(j, "gone_after_s", waited / 10.0);
    emit(c, j, h_message);
    cJSON_Delete(j);
    return EXIT_OK;
}

static int cmd_wait(ctx *c) {
    double timeout = (double)iopt(c, "timeout", 30);
    const char *exec_cmd = opt(c, "exec") ? opt(c, "exec") : getenv("ATABOY_REATTACH");
    double t = 0, last_exec = -10;
    while (t < timeout) {
        ab_devinfo devs[16];
        int n = ab_find_devices(devs, 16);
        if (n > 16) n = 16;
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++)
            if (!c->serial || !*c->serial || !strcasecmp(devs[i].serial, c->serial)) pick = i;
        if (pick < 0 && exec_cmd && *exec_cmd && t - last_exec > 3) {
            int r = system(exec_cmd);   // e.g. usbipd attach under WSL
            (void)r;
            last_exec = t;
        }
        if (pick >= 0) {
            ab_dev *d = NULL;
            const char *serial = devs[pick].serial[0] ? devs[pick].serial : NULL;
            int rc = ab_open(&d, NULL, serial, c->transport);
            cJSON *info = NULL;
            if (rc == AB_OK) rc = ab_info(d, &info);
            if (rc == AB_OK || rc == AB_ERR_PERM) {
                if (rc == AB_ERR_PERM) {
                    info = cJSON_CreateObject();
                    char fw[40];
                    snprintf(fw, sizeof(fw), "rev %s", devs[pick].rev);
                    cJSON_AddStringToObject(info, "fw", fw);
                    cJSON_AddStringToObject(info, "git", "no access to the API port");
                }
                if (d) { bootcache_save(d); ab_close(d); }
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "result", "ok");
                cJSON *dj = cJSON_AddObjectToObject(o, "device");
                cJSON_AddStringToObject(dj, "block", devs[pick].block);
                cJSON_AddStringToObject(dj, "tty", devs[pick].tty);
                cJSON_AddStringToObject(dj, "serial", devs[pick].serial);
                cJSON_AddItemToObject(o, "info", info);
                cJSON_AddNumberToObject(o, "waited_s", t);
                if (c->json) print_json(c, o);
                else printf("%s is up: %s (%s)\n", devs[pick].block[0] ? devs[pick].block : devs[pick].tty,
                            ab_jstr(info, "fw", "?"), ab_jstr(info, "git", "?"));
                cJSON_Delete(o);
                return EXIT_OK;
            }
            cJSON_Delete(info);
            ab_close(d);
        }
        sleep_ms(500);
        t += 0.5;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", "timeout");
    cJSON_AddNumberToObject(o, "waited_s", timeout);
    if (c->json) print_json(c, o); else printf("no ATAboy after %.0f s\n", timeout);
    cJSON_Delete(o);
    return EXIT_TRANSPORT;
}

static void warn_reboot(void *ctxp, uint32_t old_id, uint32_t new_id, uint8_t op) {
    (void)ctxp; (void)op;
    fprintf(stderr, "ataboy: the ATAboy rebooted (boot id %08x -> %08x): settings in RAM, geometry and "
            "mount state were reset\n", (unsigned)old_id, (unsigned)new_id);
}

static int not_yet(ctx *c) {
    fprintf(stderr, "ataboy: '%s' is not implemented in this build yet\n", c->cmd->name);
    return EXIT_USAGE;
}

static const cmd_t commands[] = {
    {"list", cmd_list, false, 0, 0, "", "list attached ATAboys (no root needed)", {{0}}},
    {"info", cmd_info, true, 0, 0, "", "firmware identity, protocol version, capabilities", {{0}}},
    {"status", cmd_status, true, 0, 0, "", "drive, geometry, mount, config, stats and last error in one call", {{0}}},
    {"config", cmd_config, true, 0, MAXPOS, "[key=value ...]", "show or change settings: config read_timeout_ms=15000 no_retry=1 --save",
     {{"save", false, "persist to flash"}}},
    {"detect", cmd_detect, true, 0, 0, "", "probe the bus, IDENTIFY, choose geometry (optionally mount)",
     {{"device", true, "auto|master|slave"}, {"geometry", true, "auto|normal|large|lba|manual|keep"},
      {"chs", true, "C/H/S for --geometry manual"}, {"no-probe", false, "skip bus reset/probe; just IDENTIFY"},
      {"force", false, "accept manual geometry if IDENTIFY fails"}, {"mount", false, "mount on success"},
      {"force-mount", false, "mount even if the kept geometry belongs to another drive"}}},
    {"geometry", cmd_geometry, true, 1, 1, "normal|large|lba|manual", "set geometry from cached IDENTIFY data or manual C/H/S",
     {{"chs", true, "C/H/S"}}},
    {"mount", cmd_mount, true, 0, 0, "", "expose the drive as USB mass storage (and rescan the host disk)",
     {{"no-rescan", false, NULL}, {"force", false, "mount even if no drive answers, it stays busy, or the geometry belongs to another drive"}}},
    {"unmount", cmd_unmount, true, 0, 0, "", "detach the drive from USB mass storage", {{"no-rescan", false, NULL}}},
    {"reset", cmd_reset, true, 0, 0, "", "soft reset (SRST); --hard pulses the RESET line and recalibrates", {{"hard", false, NULL}}},
    {"park", cmd_park, true, 0, 0, "", "park the heads before power-off (unmount first)",
     {{"landing-zone", true, "seek to this cylinder first (stepper drives)"}}},
    {"identify", cmd_identify, true, 0, 0, "", "IDENTIFY DEVICE, decoded",
     {{"cached", false, "use data from the last detect, no bus access"}, {"raw", true, "also write the 512 raw bytes"},
      {"words", false, "include all 256 words"}}},
    {"taskfile", cmd_taskfile, true, 0, 0, "", "snapshot of the ATA task-file registers", {{0}}},
    {"read", cmd_read, true, 1, 2, "LBA [COUNT]", "read sectors with per-command ATA diagnostics",
     {{"out", true, "write data (offset 0 = first LBA)"}, {"image", false, "--out is a full-disk image: write at LBA*512"},
      {"no-retry", false, "READ SECTORS without drive retries (0x21)"}, {"through-error", false, "return the drive's data for a failing sector"},
      {"timeout", true, "per-sector timeout ms (default: config)"}, {"keep-going", false, "continue past bad sectors"},
      {"hex", false, "include data as hex in JSON output"}, {"hexdump", false, "hexdump data"}, {"chunk-after-error", true, NULL}}},
    {"readlong", cmd_readlong, true, 1, 1, "LBA", "READ LONG: raw sector + ECC bytes without correction",
     {{"ecc", true, "ECC bytes (default: IDENTIFY word 22, else 4)"}, {"no-retry", false, "use 0x23"}, {"timeout", true, NULL},
      {"vote", true, "read N times and majority-vote each byte"}, {"out", true, NULL}, {"with-ecc", false, "include ECC bytes in --out"},
      {"image", true, "patch the 512 data bytes into IMG at LBA*512"}}},
    {"verify", cmd_verify, true, 2, 2, "LBA COUNT", "READ VERIFY scan (drive-internal, no data over USB); lists bad LBAs",
     {{"timeout", true, "per-command timeout ms"}, {"max-errors", true, NULL}, {"budget", true, "stop after N seconds"}}},
    {"image", not_yet, true, 1, 1, "OUT", "image the mounted drive with a ddrescue-compatible mapfile", {{"mapfile", true, NULL}, {"start", true, NULL}, {"count", true, NULL}, {"budget", true, NULL}}},
    {"rescue", not_yet, true, 2, 2, "IMAGE MAPFILE", "retry only the mapfile's bad sectors", {{"timeout", true, NULL}, {"vote", true, NULL}, {"no-readlong", false, NULL}, {"no-fast-pass", false, NULL}, {"max-sectors", true, NULL}, {"include-untried", false, NULL}, {"budget", true, NULL}, {"save-interval", true, NULL}}},
    {"corrupt", cmd_corrupt, true, 1, 1, "LBA", "DESTRUCTIVE test helper: damage a sector on purpose",
     {{"bits", true, "data bits to flip (default 16)"}, {"ecc", true, NULL}}},
    {"hashes", not_yet, true, 0, 0, "", "SHA-256 of recently read sectors", {{"since", true, NULL}, {"lba", true, NULL}, {"count", true, NULL}, {"compare", true, NULL}, {"clear", false, NULL}}},
    {"log", cmd_log, true, 0, 0, "", "error log: every failed command with ATA status/error/registers", {{"clear", false, "clear after reading"}}},
    {"clear", cmd_clear, true, 0, 0, "", "clear the error log (and/or statistics)", {{"stats", false, NULL}, {"stats-only", false, NULL}}},
    {"arm", cmd_arm, true, 0, 0, "", "allow ONE dangerous pass-through command within 10 s", {{0}}},
    {"reboot", cmd_reboot, true, 0, 0, "", "restart the firmware; --bootsel enters the UF2 bootloader", {{"bootsel", false, NULL}}},
    {"wait", cmd_wait, false, 0, 0, "", "wait until an ATAboy answers (after reboot / replug)",
     {{"timeout", true, "seconds (default 30)"}, {"exec", true, "command to run every 3 s while no device is visible (env ATABOY_REATTACH)"}}},
    {"ata", not_yet, true, 1, 1, "COMMAND", "raw ATA command via SAT pass-through", {{"feature", true, NULL}, {"count", true, NULL}, {"lba", true, NULL}, {"device", true, NULL}, {"ext", false, NULL}, {"read", true, NULL}, {"write", true, NULL}, {"out", true, NULL}, {"no-ck", false, NULL}, {"dangerous", false, NULL}, {"timeout", true, NULL}}},
    {"whatfile", not_yet, false, 1, MAXPOS, "IMAGE [LBA ...]", "map image LBAs to the FAT files that own them", {{"from-map", true, NULL}, {"from-json", true, NULL}}},
};
#define NCMDS (sizeof(commands) / sizeof(commands[0]))

static void help(void) {
    printf("usage: ataboy [-d DEV] [-s SERIAL] [-t auto|serial|sgio] [-j] [--compact] [-v] COMMAND ...\n\n"
           "Command-line client for the ATAboy automation API.  The common options may\n"
           "also follow the command.  Environment: ATABOY_DEV, ATABOY_SERIAL,\n"
           "ATABOY_TRANSPORT, ATABOY_JSON=1, ATABOY_REATTACH.\n\ncommands:\n");
    for (size_t i = 0; i < NCMDS; i++) printf("  %-10s %s\n", commands[i].name, commands[i].help);
    printf("\nataboy COMMAND --help shows the command's options.\n");
}

static void cmd_help(const cmd_t *c) {
    printf("usage: ataboy %s %s\n\n%s\n", c->name, c->pos_help, c->help);
    for (int i = 0; i < 16 && c->opts[i].name; i++)
        if (c->opts[i].help) printf("  --%-18s %s\n", c->opts[i].name, c->opts[i].help);
}

static const char *take(int argc, char **argv, int *i, const char *name, const char *inline_val) {
    if (inline_val) return inline_val;
    if (*i + 1 >= argc) usage_error("option %s needs a value", name);
    return argv[++*i];
}

int main(int argc, char **argv) {
    ctx c;
    memset(&c, 0, sizeof(c));
    c.dev = getenv("ATABOY_DEV");
    c.serial = getenv("ATABOY_SERIAL");
    c.transport = getenv("ATABOY_TRANSPORT");
    c.json = getenv("ATABOY_JSON") && !strcmp(getenv("ATABOY_JSON"), "1");
    bool want_help = false, only_pos = false;
    const char *cmdname = NULL;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (only_pos || a[0] != '-' || !a[1]) {
            if (!cmdname) {
                cmdname = a;
                for (size_t k = 0; k < NCMDS; k++) if (!strcmp(commands[k].name, a)) c.cmd = &commands[k];
                if (!c.cmd) usage_error("unknown command %s", a);
            } else {
                if (c.npos >= MAXPOS) usage_error("too many arguments");
                c.pos[c.npos++] = a;
            }
            continue;
        }
        if (!strcmp(a, "--")) { only_pos = true; continue; }
        char name[64];
        const char *inl = NULL;
        if (a[1] == '-') {
            const char *eq = strchr(a, '=');
            snprintf(name, sizeof(name), "%.*s", (int)(eq ? eq - a - 2 : (long)strlen(a) - 2), a + 2);
            if (eq) inl = eq + 1;
        } else {
            snprintf(name, sizeof(name), "%c", a[1]);
            if (a[2]) inl = a + 2;
        }
        if (!strcmp(name, "j") || !strcmp(name, "json")) { c.json = true; continue; }
        if (!strcmp(name, "compact")) { c.compact = true; continue; }
        if (!strcmp(name, "v") || !strcmp(name, "verbose")) { c.verbose = true; continue; }
        if (!strcmp(name, "no-sudo")) continue;               // accepted for compatibility
        if (!strcmp(name, "h") || !strcmp(name, "help")) { want_help = true; continue; }
        if (!strcmp(name, "version")) { printf("ataboy %s\n", VERSION); return EXIT_OK; }
        if (!strcmp(name, "d") || !strcmp(name, "dev")) { c.dev = take(argc, argv, &i, a, inl); continue; }
        if (!strcmp(name, "s") || !strcmp(name, "serial")) { c.serial = take(argc, argv, &i, a, inl); continue; }
        if (!strcmp(name, "t") || !strcmp(name, "transport")) { c.transport = take(argc, argv, &i, a, inl); continue; }
        if (!c.cmd) usage_error("unknown option %s", a);
        int k = opt_index(&c, name);
        if (k < 0) usage_error("unknown option %s", a);
        c.set[k] = true;
        if (c.cmd->opts[k].arg) c.val[k] = take(argc, argv, &i, a, inl);
        else if (inl) usage_error("option %s takes no value", a);
    }
    if (want_help) { if (c.cmd) cmd_help(c.cmd); else help(); return EXIT_OK; }
    if (!c.cmd) { help(); return EXIT_USAGE; }
    if (c.npos < c.cmd->min_pos) usage_error("missing arguments: %s", c.cmd->pos_help);
    if (c.npos > c.cmd->max_pos) usage_error("too many arguments: %s", c.cmd->pos_help);
    if (c.transport && strcmp(c.transport, "auto") && strcmp(c.transport, "serial") && strcmp(c.transport, "sgio"))
        usage_error("transport must be auto, serial or sgio (got %s)", c.transport);
    if (c.cmd->needs_dev) {
        int rc = ab_open(&c.d, c.dev, c.serial, c.transport);
        if (rc != AB_OK) { c.d = NULL; return fail(&c, rc, NULL); }
        bootcache_seed(c.d);
        ab_on_reboot(c.d, warn_reboot, NULL);
    }
    int ex = c.cmd->fn(&c);
    if (c.d) {
        bootcache_save(c.d);
        ab_close(c.d);
    }
    return ex;
}
