// The ATAboy client: opening a device, the request/reply engine and the
// operations that return JSON.

#include "internal.h"
#include "api_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
static __declspec(thread) char last_msg[AB_MSG_LEN];
#else
static _Thread_local char last_msg[AB_MSG_LEN];
#endif

void ab_vfail(const char *fmt, va_list ap) { vsnprintf(last_msg, sizeof(last_msg), fmt, ap); }

int ab_fail(int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ab_vfail(fmt, ap);
    va_end(ap);
    return err;
}

const char *ab_last_error(void) { return last_msg; }

int ab_dev_fail(ab_dev *dev, int err) {
    if (dev) snprintf(dev->msg, sizeof(dev->msg), "%s", last_msg);
    return err;
}

const char *ab_errmsg(const ab_dev *dev) { return dev ? dev->msg : last_msg; }

const char *ab_err_name(int err) {
    switch (err) {
    case AB_OK: return "ok";
    case AB_ERR_API: return "api_error";
    case AB_ERR_TRANSPORT: return "transport_error";
    case AB_ERR_NODEV: return "no_device";
    case AB_ERR_PERM: return "permission_denied";
    case AB_ERR_ARG: return "bad_argument";
    case AB_ERR_CHECK: return "check_condition";
    case AB_ERR_NOMEM: return "out_of_memory";
    case AB_ERR_IO: return "io_error";
    case AB_ERR_UNSUPPORTED: return "unsupported";
    default: return "error";
    }
}

const char *ab_result_name(int result) { return api_result_name(result); }

// ---------------------------------------------------------------------------
//  JSON helpers
// ---------------------------------------------------------------------------

cJSON *ab_jobj_get(const cJSON *o, const char *key) {
    return o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}
bool ab_jbool(const cJSON *o, const char *key, bool dflt) {
    const cJSON *v = ab_jobj_get(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}
double ab_jnum(const cJSON *o, const char *key, double dflt) {
    const cJSON *v = ab_jobj_get(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
const char *ab_jstr(const cJSON *o, const char *key, const char *dflt) {
    const cJSON *v = ab_jobj_get(o, key);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

// ---------------------------------------------------------------------------
//  Open / close
// ---------------------------------------------------------------------------

static bool select_device(const char *serial, ab_devinfo *out) {
    ab_devinfo devs[16];
    int n = ab_find_devices(devs, 16);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        if (serial && *serial && strcasecmp(devs[i].serial, serial) != 0) continue;
        *out = devs[i];
        return true;
    }
    if (serial && *serial) ab_fail(AB_ERR_NODEV, "no ATAboy with USB serial %s", serial);
    else ab_fail(AB_ERR_NODEV, "no ATAboy found (is it plugged in%s?)",
#if defined(__linux__)
                 ", and under WSL attached with usbipd"
#else
                 ""
#endif
                 );
    return false;
}

static bool match_path(const char *path, ab_devinfo *out) {
    ab_devinfo devs[16];
    int n = ab_find_devices(devs, 16);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++)
        if (strcmp(devs[i].tty, path) == 0 || strcmp(devs[i].block, path) == 0 ||
            strcmp(devs[i].sg, path) == 0) {
            *out = devs[i];
            return true;
        }
    return false;
}

int ab_open(ab_dev **out, const char *path, const char *serial, const char *transport) {
    *out = NULL;
    if (!transport || !*transport) transport = "auto";
    bool want_serial = strcmp(transport, "serial") == 0, want_sgio = strcmp(transport, "sgio") == 0;
    if (!want_serial && !want_sgio && strcmp(transport, "auto") != 0)
        return ab_fail(AB_ERR_ARG, "transport must be auto, serial or sgio");
    ab_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) return ab_fail(AB_ERR_NOMEM, "out of memory");
    dev->ext = -1;
    if (path && *path) dev->have_info = match_path(path, &dev->info);
    else if (!(dev->have_info = select_device(serial, &dev->info))) { free(dev); return AB_ERR_NODEV; }

    const char *tty = NULL;
    if (path && *path && ab_is_serial_path(path)) {
        if (want_sgio) { free(dev); return ab_fail(AB_ERR_ARG, "%s is a serial port; SG_IO needs /dev/sgN or /dev/sdX", path); }
        tty = path;
    } else if (!want_sgio && dev->have_info && dev->info.tty[0] && (!path || !*path || want_serial)) {
        tty = dev->info.tty;
    }
    if (want_serial && !tty) {
        free(dev);
        return ab_fail(AB_ERR_NODEV, "no ATAboy API serial port (firmware without the serial_api feature?)");
    }
    int rc = AB_ERR_NODEV;
    if (tty) {
        rc = ab_serial_transport_open(tty, &dev->tp);
        if (rc == AB_OK) {
            snprintf(dev->path, sizeof(dev->path), "%s", tty);
            *out = dev;
            return AB_OK;
        }
        if (want_serial) { free(dev); return rc; }
    }
#if defined(__linux__)
    const char *sg = (path && *path && !ab_is_serial_path(path)) ? path
                     : dev->have_info ? (dev->info.sg[0] ? dev->info.sg : dev->info.block) : NULL;
    if (sg && *sg) {
        int rc2 = ab_sgio_transport_open(sg, &dev->tp);
        if (rc2 == AB_OK) {
            snprintf(dev->path, sizeof(dev->path), "%s", sg);
            *out = dev;
            return AB_OK;
        }
        // Report the serial-port problem if there was one: it's the path that
        // needs no root, so its error is the actionable one.
        if (!tty) rc = rc2;
    } else if (!tty) {
        rc = ab_fail(AB_ERR_NODEV, "ATAboy %s has no SCSI disk device", dev->info.serial);
    }
#else
    if (!tty) rc = ab_fail(AB_ERR_UNSUPPORTED, "no API serial port found for this ATAboy");
#endif
    free(dev);
    return rc;
}

void ab_close(ab_dev *dev) {
    if (!dev) return;
    if (dev->tp) dev->tp->close(dev->tp);
    free(dev);
}

const ab_devinfo *ab_dev_info(const ab_dev *dev) { return dev && dev->have_info ? &dev->info : NULL; }
const char *ab_transport_name(const ab_dev *dev) { return dev && dev->tp ? dev->tp->name : "none"; }
void ab_sense(const ab_dev *dev, uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    *key = dev->sense[0]; *asc = dev->sense[1]; *ascq = dev->sense[2];
}

void ab_on_reboot(ab_dev *dev, ab_reboot_cb cb, void *ctx) { dev->on_reboot = cb; dev->on_reboot_ctx = ctx; }
void ab_seed_boot_id(ab_dev *dev, uint32_t id) { if (!dev->have_boot_id) { dev->boot_id = id; dev->have_boot_id = true; } }
bool ab_boot_id(const ab_dev *dev, uint32_t *id) { if (dev->have_boot_id) *id = dev->boot_id; return dev->have_boot_id; }
const char *ab_dev_key(const ab_dev *dev) {
    return dev->have_info && dev->info.serial[0] ? dev->info.serial : dev->path;
}

cJSON *ab_link_info(const ab_dev *dev) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "transport", ab_transport_name(dev));
    cJSON_AddStringToObject(j, "path", dev->path);
    if (dev->have_info && dev->info.serial[0]) cJSON_AddStringToObject(j, "serial", dev->info.serial);
    else cJSON_AddNullToObject(j, "serial");
    cJSON_AddBoolToObject(j, "extended", dev->ext == 1);
    if (dev->ext == 1 && dev->have_boot_id) {
        cJSON_AddNumberToObject(j, "boot_id", dev->boot_id);
        cJSON_AddNumberToObject(j, "state_counter", dev->state_counter);
    } else {
        cJSON_AddNullToObject(j, "boot_id");
        cJSON_AddNullToObject(j, "state_counter");
    }
    cJSON_AddNumberToObject(j, "reboots_seen", dev->reboots);
    if (dev->have_last_reboot) {
        cJSON *r = cJSON_AddObjectToObject(j, "last_reboot");
        cJSON_AddNumberToObject(r, "old_boot_id", dev->last_old_boot);
        cJSON_AddNumberToObject(r, "new_boot_id", dev->last_new_boot);
        cJSON_AddNumberToObject(r, "op", dev->last_reboot_op);
        cJSON_AddNumberToObject(r, "uptime_ms", dev->last_reboot_uptime);
    } else {
        cJSON_AddNullToObject(j, "last_reboot");
    }
    return j;
}

// ---------------------------------------------------------------------------
//  Request / reply
// ---------------------------------------------------------------------------

void ab_reply_free(ab_reply *r) {
    if (r && r->json) { cJSON_Delete(r->json); r->json = NULL; }
}

static int parse_reply(ab_reply *r) {
    const uint8_t *b = r->raw;
    if (r->raw_len < HDR_LEN) return ab_fail(AB_ERR_TRANSPORT, "short reply (%zu bytes)", r->raw_len);
    if (b[0] != 'A' || b[1] != 'B')
        return ab_fail(AB_ERR_TRANSPORT, "bad reply magic %02x %02x: is the API firmware loaded?", b[0], b[1]);
    r->proto = b[2];
    r->op = b[3];
    r->result = b[4];
    r->ctype = b[5];
    size_t plen = ab_be16(&b[6]);
    r->uptime_ms = ab_be32(&b[8]);
    r->flags = b[12];
    size_t hdr = b[13] == EXT_HDR_LEN ? EXT_HDR_LEN : HDR_LEN;
    r->ext = hdr == EXT_HDR_LEN;
    if (r->ext) {
        if (r->raw_len < EXT_HDR_LEN) return ab_fail(AB_ERR_TRANSPORT, "short extended reply (%zu bytes)", r->raw_len);
        r->boot_id = ab_be32(&b[16]);
        r->state_counter = ab_be32(&b[20]);
        r->request_id = ab_be32(&b[24]);
        r->replayed = (b[28] & XF_REPLAYED) != 0;
        r->rid_taken = (b[28] & XF_RID) != 0;
    }
    if (r->raw_len < hdr + plen)
        return ab_fail(AB_ERR_TRANSPORT, "truncated reply: %zu of %zu payload bytes", r->raw_len - hdr, plen);
    r->payload = b + hdr;
    r->payload_len = plen;
    if (r->ctype == CT_JSON) {
        r->json = cJSON_ParseWithLength((const char *)r->payload, plen);
        if (!r->json) return ab_fail(AB_ERR_TRANSPORT, "device sent malformed JSON");
    }
    return AB_OK;
}

static int transact(ab_dev *dev, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms,
                    ab_reply *r, bool *maybe_executed) {
    r->raw_len = 0;
    if (alloc > sizeof(r->raw)) alloc = sizeof(r->raw);
    int rc = dev->tp->api(dev->tp, cdb, alloc, timeout_ms, r->raw, &r->raw_len, maybe_executed, dev->sense);
    return rc;
}

// Send; send once more after an empty or lost reply when that is safe
// (resend: read-only op or a request id, or the device never ran it).
static int send_req(ab_dev *dev, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms, bool resend,
                    ab_reply *r) {
    bool maybe = false;
    int rc = transact(dev, cdb, alloc, timeout_ms, r, &maybe);
    if (rc == AB_ERR_TRANSPORT) {
        if (maybe && !resend) return rc;
        r->raw_len = 0;
        resend = true;
    } else if (rc != AB_OK) {
        return rc;
    }
    if (r->raw_len == 0 && resend) {
        // Rarely a reply arrives with GOOD status and no data.  Read-only ops
        // are simply asked again; a state-changing op goes again with the same
        // request id and the firmware answers from its replay store.
        dev->empty_replies++;
        rc = transact(dev, cdb, alloc, timeout_ms, r, &maybe);
    }
    return rc;
}

static void note_reply(ab_dev *dev, const ab_reply *r_in, uint8_t op) {
    ab_reply *r = (ab_reply *)r_in;
    if (!r->ext) return;
    if (dev->have_boot_id && dev->boot_id != r->boot_id) {
        r->rebooted = true;
        dev->reboots++;
        dev->have_last_reboot = true;
        dev->last_old_boot = dev->boot_id;
        dev->last_new_boot = r->boot_id;
        dev->last_reboot_op = op;
        dev->last_reboot_uptime = r->uptime_ms;
        if (dev->on_reboot) dev->on_reboot(dev->on_reboot_ctx, dev->boot_id, r->boot_id, op);
    }
    dev->have_boot_id = true;
    dev->boot_id = r->boot_id;
    dev->state_counter = r->state_counter;
}

int ab_call(ab_dev *dev, uint8_t op, const uint8_t *args, size_t nargs, uint32_t alloc, uint32_t timeout_ms,
            ab_reply *r) {
    memset(r, 0, offsetof(ab_reply, raw));
    r->raw_len = 0;
    if (nargs > 12) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "at most 12 argument bytes"));
    uint8_t a[12] = {0};
    if (nargs) memcpy(a, args, nargs);
    bool ext = dev->ext != 0;
    uint32_t rid = 0;
    if (ext && api_op_takes_rid(op)) {
        if (a[8] || a[9] || a[10] || a[11])
            return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "op 0x%02x arguments overlap the request id", op));
        do rid = ab_random32(); while (rid == 0);
        ab_put_be32(&a[8], rid);
    }
    uint8_t cdb[16] = {API_OPCODE, op, API_SIG0, (uint8_t)(ext ? API_SIG_EXT : API_SIG_BASIC)};
    memcpy(cdb + 4, a, 12);
    int rc = send_req(dev, cdb, alloc ? alloc : API_MAX_REPLY, timeout_ms ? timeout_ms : 60000,
                      api_op_read_only(op) || rid != 0, r);
    // (5/24/00 also means "allocation too small", so only an adequate
    // allocation makes it evidence of old firmware.)
    if (rc == AB_ERR_CHECK && ext && dev->ext == -1 && dev->sense[0] == 5 && dev->sense[1] == 0x24 &&
        dev->sense[2] == 0 && (alloc == 0 || alloc >= API_MIN_ALLOC)) {
        // Firmware without the extended header refuses the signature before
        // running anything: basic requests from now on.
        dev->ext = 0;
        if (rid) memset(a + 8, 0, 4);
        return ab_call(dev, op, a, 12, alloc, timeout_ms, r);
    }
    if (rc != AB_OK) return ab_dev_fail(dev, rc);
    if (r->raw_len == 0) return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "empty reply from the ATAboy"));
    rc = parse_reply(r);
    if (rc != AB_OK) { ab_reply_free(r); return ab_dev_fail(dev, rc); }
    if (r->proto != API_PROTO_VERSION) {
        ab_reply_free(r);
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "device speaks API protocol %u, this client %u",
                                        r->proto, API_PROTO_VERSION));
    }
    if (ext && r->ext) dev->ext = 1;
    note_reply(dev, r, op);
    return AB_OK;
}

// A JSON op: *out gets the reply JSON (detached from the reply), result
// checked.  sub: return this member instead of the whole object.
static int json_op(ab_dev *dev, uint8_t op, const uint8_t *args, size_t nargs, uint32_t timeout_ms,
                   cJSON **out, const char *sub) {
    if (out) *out = NULL;
    ab_reply r;
    int rc = ab_call(dev, op, args, nargs, 0, timeout_ms, &r);
    if (rc != AB_OK) return rc;
    if (!r.json) {
        int res = r.result;
        ab_reply_free(&r);
        if (res != R_OK) {
            if (out) {
                *out = cJSON_CreateObject();
                cJSON_AddStringToObject(*out, "result", api_result_name(res));
            }
            ab_fail(AB_ERR_API, "%s", api_result_name(res));
            return ab_dev_fail(dev, AB_ERR_API);
        }
        return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "expected JSON from %s", api_op_name(op)));
    }
    cJSON *j = r.json;
    r.json = NULL;
    int res = r.result;
    if (res != R_OK) {
        const char *m = ab_jstr(j, "message", "");
        ab_fail(AB_ERR_API, "%s%s%s", api_result_name(res), *m ? ": " : "", m);
        if (out) *out = j; else cJSON_Delete(j);
        return ab_dev_fail(dev, AB_ERR_API);
    }
    if (sub) {
        cJSON *s = cJSON_DetachItemFromObjectCaseSensitive(j, sub);
        cJSON_Delete(j);
        if (!s) return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "reply lacks \"%s\"", sub));
        j = s;
    }
    if (out) *out = j; else cJSON_Delete(j);
    return AB_OK;
}

// ---------------------------------------------------------------------------
//  Kernel / OS view of the USB disk
// ---------------------------------------------------------------------------

#define MSC_CHUNK_SECTORS 8   // one USB-disk READ(10) chunk: the firmware's 4 KiB buffer

// Kernel SCSI timeout covering one READ(10) chunk of slow-but-good sectors,
// each allowed the firmware's per-sector timeout, plus 30 s.
static int kernel_timeout_needed(double fw_timeout_ms) {
    long long ms = (long long)fw_timeout_ms * MSC_CHUNK_SECTORS;
    return (int)((ms + 999) / 1000 + 30);
}

static void sync_kernel_timeout(ab_dev *dev, const cJSON *status) {
    if (!dev->have_info) return;
    const cJSON *cfg = ab_jobj_get(status, "config");
    double ms = ab_jnum(cfg, "read_timeout_ms", 0);
    if (ms > 0) ab_os_ensure_scsi_timeout(&dev->info, kernel_timeout_needed(ms));
}

static void rescan(ab_dev *dev, int want_ro, uint32_t wait_ms) {
    if (!dev->have_info || !dev->info.block[0]) return;
    cJSON *st = NULL;
    if (ab_status(dev, &st) != AB_OK) { cJSON_Delete(st); return; }
    uint64_t want = ab_jbool(st, "mounted", false)
                    ? (uint64_t)ab_jnum(ab_jobj_get(st, "geometry"), "sectors", 0) : 0;
    cJSON_Delete(st);
    ab_os_rescan(&dev->info, want, want_ro, wait_ms);
}

// ---------------------------------------------------------------------------
//  JSON operations
// ---------------------------------------------------------------------------

int ab_info(ab_dev *dev, cJSON **out) {
    int rc = json_op(dev, OP_INFO, NULL, 0, 0, out, NULL);
    if (rc == AB_OK && out && *out) dev->max_read_sectors = (int)ab_jnum(*out, "max_read_sectors", 7);
    return rc;
}
int ab_status(ab_dev *dev, cJSON **out) { return json_op(dev, OP_STATUS, NULL, 0, 0, out, NULL); }
int ab_config_get(ab_dev *dev, cJSON **out) { return json_op(dev, OP_CONFIG_GET, NULL, 0, 0, out, "config"); }
int ab_config_save(ab_dev *dev, cJSON **out) { return json_op(dev, OP_CONFIG_SAVE, NULL, 0, 0, out, NULL); }
int ab_config_defaults(ab_dev *dev, cJSON **out) { return json_op(dev, OP_CONFIG_DEFAULTS, NULL, 0, 0, out, NULL); }

const char *const ab_config_keys[] = {
    "write_protect", "auto_mount", "iordy", "intrq", "read_timeout_ms", "write_timeout_ms",
    "ecc_len", "no_retry", "device", NULL,
};
static const uint8_t config_key_nums[] = {
    KEY_WRITE_PROTECT, KEY_AUTO_MOUNT, KEY_IORDY, KEY_INTRQ, KEY_READ_TIMEOUT, KEY_WRITE_TIMEOUT,
    KEY_ECC_LEN, KEY_NO_RETRY, KEY_DEVICE,
};

static int parse_bool(const char *v, uint32_t *out) {
    static const char *const t[] = {"1", "on", "true", "yes", "enabled", NULL};
    static const char *const f[] = {"0", "off", "false", "no", "disabled", NULL};
    for (int i = 0; t[i]; i++) if (strcasecmp(v, t[i]) == 0) { *out = 1; return 0; }
    for (int i = 0; f[i]; i++) if (strcasecmp(v, f[i]) == 0) { *out = 0; return 0; }
    return -1;
}

static int parse_u32(const char *v, uint32_t *out) {
    char *end;
    unsigned long long x = strtoull(v, &end, 0);
    if (!*v || *end || x > 0xFFFFFFFFull || v[0] == '-') return -1;
    *out = (uint32_t)x;
    return 0;
}

int ab_config_set(ab_dev *dev, const char *key, const char *value, cJSON **out) {
    if (out) *out = NULL;
    int k = -1;
    for (int i = 0; ab_config_keys[i]; i++) if (strcmp(ab_config_keys[i], key) == 0) k = i;
    if (k < 0) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "unknown setting %s", key));
    uint8_t num = config_key_nums[k];
    uint32_t v;
    bool ok;
    if (num == KEY_DEVICE) {
        ok = true;
        if (strcasecmp(value, "master") == 0 || strcmp(value, "0") == 0) v = 0;
        else if (strcasecmp(value, "slave") == 0 || strcmp(value, "1") == 0) v = 1;
        else ok = false;
    } else if (num == KEY_READ_TIMEOUT || num == KEY_WRITE_TIMEOUT || num == KEY_ECC_LEN) {
        ok = parse_u32(value, &v) == 0;
    } else {
        ok = parse_bool(value, &v) == 0;
    }
    if (!ok) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "bad value %s for %s", value, key));
    uint8_t a[8] = {num, 0, 0, 0};
    ab_put_be32(&a[4], v);
    cJSON *j = NULL;
    int rc = json_op(dev, OP_CONFIG_SET, a, sizeof(a), 0, &j, NULL);
    if (rc != AB_OK) { if (out) *out = j; else cJSON_Delete(j); return rc; }
    cJSON *cfg = cJSON_DetachItemFromObjectCaseSensitive(j, "config");
    cJSON_Delete(j);
    if (!cfg) return ab_dev_fail(dev, ab_fail(AB_ERR_TRANSPORT, "reply lacks \"config\""));
    if (num == KEY_READ_TIMEOUT && dev->have_info) ab_os_ensure_scsi_timeout(&dev->info, kernel_timeout_needed(v));
    if (num == KEY_WRITE_PROTECT) rescan(dev, ab_jbool(cfg, "write_protect", true) ? 1 : 0, 3000);
    if (out) *out = cfg; else cJSON_Delete(cfg);
    return AB_OK;
}

static int geo_mode(const char *m) {
    static const char *const names[] = {"auto", "normal", "large", "lba", "manual", "keep", NULL};
    if (!m || !*m) return APIGEO_AUTO;
    for (int i = 0; names[i]; i++) if (strcmp(names[i], m) == 0) return i;
    return -1;
}

int ab_detect(ab_dev *dev, const ab_detect_opts *o, cJSON **out) {
    if (out) *out = NULL;
    ab_detect_opts d = {0};
    if (o) d = *o;
    int devsel = !d.device || !strcmp(d.device, "auto") ? 0 : !strcmp(d.device, "master") ? 1
               : !strcmp(d.device, "slave") ? 2 : -1;
    int geo = geo_mode(d.geometry);
    if (devsel < 0) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "device must be auto, master or slave"));
    if (geo < 0) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "unknown geometry mode %s", d.geometry));
    if (d.cyls > 65535 || d.heads > 255 || d.spt > 255) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "CHS out of range"));
    uint8_t a[7] = {(uint8_t)devsel, (uint8_t)geo, 0, 0, (uint8_t)d.heads, (uint8_t)d.spt,
                    (uint8_t)((d.no_probe ? 1 : 0) | (d.mount || d.force_mount ? 2 : 0) | (d.force ? 4 : 0) |
                              (d.force_mount ? 8 : 0))};
    ab_put_be16(&a[2], (uint16_t)d.cyls);
    cJSON *j = NULL;
    int rc = json_op(dev, OP_DETECT, a, sizeof(a), 120000, &j, NULL);
    if (j) sync_kernel_timeout(dev, j);          // before the rescan makes the kernel read the disk
    if (rc == AB_OK && (d.mount || d.force_mount)) rescan(dev, -1, 5000);
    if (out) *out = j; else cJSON_Delete(j);
    return rc;
}

int ab_geometry(ab_dev *dev, const char *mode, unsigned c, unsigned h, unsigned s, cJSON **out) {
    int geo = geo_mode(mode);
    if (geo < 1 || geo > 4) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "geometry mode must be normal, large, lba or manual"));
    if (c > 65535 || h > 255 || s > 255) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "CHS out of range"));
    uint8_t a[6] = {0, (uint8_t)geo, 0, 0, (uint8_t)h, (uint8_t)s};
    ab_put_be16(&a[2], (uint16_t)c);
    return json_op(dev, OP_GEOMETRY, a, sizeof(a), 0, out, NULL);
}

int ab_mount(ab_dev *dev, bool force, cJSON **out) {
    uint8_t a[1] = {force ? 1 : 0};
    cJSON *j = NULL;
    int rc = json_op(dev, OP_MOUNT, a, 1, 0, &j, NULL);
    if (j) sync_kernel_timeout(dev, j);
    if (rc == AB_OK) rescan(dev, -1, 5000);
    if (out) *out = j; else cJSON_Delete(j);
    return rc;
}

int ab_unmount(ab_dev *dev, cJSON **out) {
    int rc = json_op(dev, OP_UNMOUNT, NULL, 0, 0, out, NULL);
    if (rc == AB_OK) rescan(dev, -1, 5000);
    return rc;
}

int ab_reset(ab_dev *dev, bool hard, cJSON **out) {
    uint8_t a[1] = {hard ? 1 : 0};
    // firmware worst case: 31 s wait + RESET-line fallback with another 31 s
    return json_op(dev, OP_RESET, a, 1, 90000, out, NULL);
}

int ab_park(ab_dev *dev, int cylinder, cJSON **out) {
    if (cylinder > 65535) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "landing-zone cylinder must be 0..65535"));
    uint8_t a[4] = {cylinder >= 0 ? 1 : 0, 0, 0, 0};
    ab_put_be16(&a[2], (uint16_t)(cylinder >= 0 ? cylinder : 0));
    return json_op(dev, OP_PARK, a, sizeof(a), 90000, out, NULL);
}

int ab_taskfile(ab_dev *dev, cJSON **out) { return json_op(dev, OP_TASKFILE, NULL, 0, 0, out, NULL); }

int ab_log(ab_dev *dev, cJSON **out) {
    // Pages through the device log: {"total", "retained", "entries"}.
    if (out) *out = NULL;
    cJSON *entries = cJSON_CreateArray();
    double total = 0;
    uint32_t start = 0;
    for (int page = 0; page < 10000; page++) {
        uint8_t a[4];
        ab_put_be32(a, start);
        cJSON *j = NULL;
        int rc = json_op(dev, OP_LOG, a, 4, 0, &j, NULL);
        if (rc != AB_OK) { cJSON_Delete(entries); if (out) *out = j; else cJSON_Delete(j); return rc; }
        total = ab_jnum(j, "total", 0);
        cJSON *e = ab_jobj_get(j, "entries"), *x;
        while (e && (x = cJSON_DetachItemFromArray(e, 0)) != NULL) cJSON_AddItemToArray(entries, x);
        const cJSON *next = ab_jobj_get(j, "next");
        double returned = ab_jnum(j, "returned", 0);
        bool more = cJSON_IsNumber(next) && returned > 0 && (uint32_t)next->valuedouble > start;
        if (more) start = (uint32_t)next->valuedouble;
        cJSON_Delete(j);
        if (!more) break;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "retained", cJSON_GetArraySize(entries));
    cJSON_AddItemToObject(o, "entries", entries);
    if (out) *out = o; else cJSON_Delete(o);
    return AB_OK;
}

int ab_clear(ab_dev *dev, bool log, bool stats, cJSON **out) {
    uint8_t a[1] = {(uint8_t)((log ? 1 : 0) | (stats ? 2 : 0))};
    return json_op(dev, OP_LOG_CLEAR, a, 1, 0, out, NULL);
}

int ab_arm(ab_dev *dev, cJSON **out) {
    const uint8_t a[4] = {'A', 'R', 'M', '!'};
    return json_op(dev, OP_ARM, a, 4, 0, out, NULL);
}

int ab_reboot(ab_dev *dev, int mode, cJSON **out) {
    if (mode < 0 || mode > 2) return ab_dev_fail(dev, ab_fail(AB_ERR_ARG, "reboot mode must be 0, 1 or 2"));
    const uint8_t a[8] = {(uint8_t)mode, 0, 0, 0, 'B', 'O', 'O', 'T'};
    return json_op(dev, OP_REBOOT, a, 8, 0, out, NULL);
}
