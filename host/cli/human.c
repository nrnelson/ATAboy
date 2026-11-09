// Human-readable output, same layout as the Python tool's human_* functions.

#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *get(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static double num(const cJSON *o, const char *k) { const cJSON *v = get(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static long long inum(const cJSON *o, const char *k) { return (long long)num(o, k); }
static const char *str(const cJSON *o, const char *k) { const cJSON *v = get(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
static bool tru(const cJSON *o, const char *k) { return cJSON_IsTrue(get(o, k)); }
static bool has(const cJSON *o, const char *k) { const cJSON *v = get(o, k); return v && !cJSON_IsNull(v); }

void human_log_entry(const cJSON *x, const char *prefix) {
    printf("%s#%-4lld t=%9.3fs %-10s cmd %02llX LBA %-9lld fail %-9lld st %02llX[%s] err %02llX[%s] %s %lld/%lld ms%s\n",
           prefix, inum(x, "seq"), num(x, "t_ms") / 1000, str(x, "op"), inum(x, "cmd"), inum(x, "lba"),
           inum(x, "fail_lba"), inum(x, "status"), str(x, "status_bits"), inum(x, "error"), str(x, "error_bits"),
           str(x, "result"), inum(x, "elapsed_ms"), inum(x, "timeout_ms"),
           tru(x, "tf_valid") ? "" : " (regs invalid: BSY)");
}

void human_status(const cJSON *j) {
    const cJSON *d = get(j, "drive"), *g = get(j, "geometry"), *st = get(j, "settings");
    printf("firmware   %s  (uptime %.1f s)\n", str(j, "fw"), num(j, "uptime_ms") / 1000);
    if (!strcmp(str(st, "load"), "crc_error") || !strcmp(str(st, "load"), "invalid"))
        printf("settings   SAVED SETTINGS REJECTED (%s%s%s): defaults loaded\n", str(st, "load"),
               has(st, "field") ? ", field " : "", has(st, "field") ? str(st, "field") : "");
    if (tru(d, "identified") || tru(d, "forced")) {
        const char *s = str(d, "serial"), *f = str(d, "firmware");
        printf("drive      %s  serial %s  fw %s  [%s]\n", str(d, "model"), *s ? s : "-", *f ? f : "-", str(d, "device"));
    } else {
        printf("drive      %s\n", tru(d, "present") ? "present, not identified" : "not detected");
    }
    if (tru(g, "valid")) {
        printf("geometry   %s %s  %lld/%lld/%lld  %lld sectors (%.1f MB)\n", str(g, "mode"), str(g, "addressing"),
               inum(g, "cyls"), inum(g, "heads"), inum(g, "spt"), inum(g, "sectors"), num(g, "sectors") * 512 / 1e6);
        if (!strcmp(str(g, "identity"), "mismatch")) {
            const cJSON *o = get(g, "owner");
            const char *m = str(o, "model"), *s = str(o, "serial");
            printf("           MISMATCH: set up for %s (serial %s), not the drive attached; detect again\n",
                   *m ? m : "a drive with different IDENTIFY data", *s ? s : "-");
        }
    } else {
        printf("geometry   not set\n");
    }
    const cJSON *c = get(j, "config");
    printf("state      %s, write protect %s, read timeout %lld ms%s%s\n", tru(j, "mounted") ? "MOUNTED" : "not mounted",
           tru(j, "write_protect") ? "ON" : "OFF", inum(c, "read_timeout_ms"),
           tru(c, "no_retry") ? ", no-retry reads" : "", tru(j, "armed") ? ", ARMED" : "");
    const cJSON *b = get(j, "bus");
    printf("bus        status %02llX %s\n", inum(b, "status"), str(b, "status_bits"));
    const cJSON *s = get(j, "stats");
    printf("stats      reads %lld (%lld sectors, %lld errors)  writes %lld (%lld errors)  timeouts %lld  recoveries %lld\n",
           inum(s, "read_cmds"), inum(s, "read_sectors"), inum(s, "read_errors"), inum(s, "write_cmds"),
           inum(s, "write_errors"), inum(s, "timeouts"), inum(s, "recoveries"));
    const cJSON *e = get(j, "errors");
    printf("errors     %lld logged\n", inum(e, "total"));
    if (has(e, "last")) human_log_entry(get(e, "last"), "  last     ");
}

void human_ata_result(const cJSON *d) {
    char up[32];
    snprintf(up, sizeof(up), "%s", str(d, "result"));
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    char fl[40] = "";
    if (has(d, "fail_lba")) snprintf(fl, sizeof(fl), "  fail_lba %lld", inum(d, "fail_lba"));
    printf("%s  LBA %lld  %lld/%lld sectors  st %02llX[%s] err %02llX[%s]  %.1f ms%s%s%s\n", up, inum(d, "lba"),
           inum(d, "done"), inum(d, "count"), inum(d, "status"), str(d, "status_bits"), inum(d, "error"),
           str(d, "error_bits"), num(d, "elapsed_ms"), fl, tru(d, "err_data") ? "  (data returned despite error)" : "",
           tru(d, "timed_out") ? "  TIMEOUT" : "");
}

void human_park(const cJSON *j) {
    printf("park: %s%s%s\n", str(j, "result"), has(j, "message") ? " - " : "", str(j, "message"));
    static const char *const keys[] = {"seek", "standby"};
    for (int i = 0; i < 2; i++) {
        const cJSON *c = get(j, keys[i]);
        if (!cJSON_IsObject(c)) continue;
        char cyl[32] = "";
        if (has(c, "cylinder")) snprintf(cyl, sizeof(cyl), "  cylinder %lld", inum(c, "cylinder"));
        printf("  %-8s cmd %02llX %-9s st %02llX[%s] err %02llX[%s] %lld ms%s\n", keys[i], inum(c, "command"),
               str(c, "result"), inum(c, "status"), str(c, "status_bits"), inum(c, "error"), str(c, "error_bits"),
               inum(c, "elapsed_ms"), cyl);
    }
    if (tru(j, "fallback"))
        printf("  E0h was aborted (st %02llX err %02llX); sent the ATA-1 STANDBY IMMEDIATE 94h\n",
               inum(j, "e0_status"), inum(j, "e0_error"));
}

void human_taskfile(const cJSON *j) {
    printf("ERR %02llX[%s] CNT %02llX LBA %02llX/%02llX/%02llX DEV %02llX ST %02llX[%s]%s\n", inum(j, "error"),
           str(j, "error_bits"), inum(j, "count"), inum(j, "lba_lo"), inum(j, "lba_mid"), inum(j, "lba_hi"),
           inum(j, "device"), inum(j, "status"), str(j, "status_bits"), tru(j, "valid") ? "" : "  (BSY: registers invalid)");
}

// "key  value" lines; values printed as compact JSON (strings bare).
void human_keyvals(const cJSON *j, const char *skip) {
    for (const cJSON *c = j ? j->child : NULL; c; c = c->next) {
        if (skip && c->string && !strcmp(c->string, skip)) continue;
        if (cJSON_IsString(c)) { printf("%-20s %s\n", c->string, c->valuestring); continue; }
        if (cJSON_IsBool(c)) { printf("%-20s %s\n", c->string, cJSON_IsTrue(c) ? "True" : "False"); continue; }
        if (cJSON_IsNull(c)) { printf("%-20s None\n", c->string); continue; }
        char *s = ab_json_print(c, 0);
        printf("%-20s %s\n", c->string, s ? s : "?");
        free(s);
    }
}

void hexdump(const uint8_t *data, size_t n, uint64_t base) {
    for (size_t off = 0; off < n; off += 16) {
        char hx[64] = "", asc[17];
        size_t k = 0, m = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < m; i++) {
            k += (size_t)snprintf(hx + k, sizeof(hx) - k, i ? " %02x" : "%02x", data[off + i]);
            asc[i] = (data[off + i] >= 32 && data[off + i] < 127) ? (char)data[off + i] : '.';
        }
        asc[m] = '\0';
        printf("%08llx  %-47s  %s\n", (unsigned long long)(base + off), hx, asc);
    }
}

// ---------------------------------------------------------------------------
//  whatfile
// ---------------------------------------------------------------------------

// Print a library string; C0 80 is how it carries U+0000 (fatmap.c).
static void put_text(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)s[0] == 0xC0 && (unsigned char)s[1] == 0x80) { putchar('\0'); s++; }
        else putchar(*s);
    }
}

static void entries_out(const cJSON *ents) {
    int n = cJSON_GetArraySize(ents), i = 0;
    if (!n) { fputs("no entries in this sector", stdout); return; }
    fputs("entries: ", stdout);
    for (const cJSON *e = ents->child; e && i < 6; e = e->next, i++)
        printf("%s%s%s%s%s", i ? ", " : "", str(e, "name"), tru(e, "dir") ? "/" : "",
               tru(e, "deleted") ? " (deleted)" : "", tru(e, "label") ? " (label)" : "");
    if (n > 6) printf(", +%d more", n - 6);
}

void human_whatfile_line(const cJSON *r) {
    const char *k = str(r, "kind");
    char where[96];
    snprintf(where, sizeof(where), "%s %s", has(r, "partition") ? str(r, "partition") : "-", str(r, "fs"));
    size_t wl = strlen(where);
    while (wl && where[wl - 1] == ' ') where[--wl] = '\0';
    printf("%10lld  %-10s ", inum(r, "lba"), where);
    if (!strcmp(k, "file")) {
        printf("file %s  [offset %lld of %lld bytes%s, cluster %lld]", str(r, "path"), inum(r, "offset"),
               inum(r, "size"), tru(r, "past_eof") ? ", past EOF (slack)" : "", inum(r, "cluster"));
    } else if (!strcmp(k, "directory") || !strcmp(k, "root_dir")) {
        printf("%s %s  [", !strcmp(k, "directory") ? "directory" : "root directory", str(r, "path"));
        entries_out(get(r, "entries"));
        printf("]");
    } else if (!strcmp(k, "deleted")) {
        printf("deleted (best effort) %s%s  [cluster %lld, %s; ", str(r, "path"), tru(r, "is_dir") ? "/" : "",
               inum(r, "cluster"), str(r, "detail"));
        if (tru(r, "is_dir")) entries_out(get(r, "entries"));
        else printf("offset %lld of %lld bytes%s", inum(r, "offset"), inum(r, "size"),
                    tru(r, "past_eof") ? ", past EOF (slack)" : "");
        printf("]");
    } else if (!strcmp(k, "fat")) {
        const cJSON *c = get(r, "clusters");
        const cJSON *c0 = cJSON_GetArrayItem(c, 0), *c1 = cJSON_GetArrayItem(c, 1);
        printf("FAT copy %lld of %lld (entries for clusters %lld-%lld)%s%s", inum(r, "fat_copy"), inum(r, "fats"),
               c0 ? (long long)c0->valuedouble : 0, c1 ? (long long)c1->valuedouble : 0,
               has(r, "detail") ? "; " : "", str(r, "detail"));
    } else if (!strcmp(k, "non_fat_partition")) {
        printf("non-FAT partition (type 0x%02llX %s)", inum(r, "type"), str(r, "type_name"));
    } else {
        for (const char *p = k; *p; p++) putchar(*p == '_' ? ' ' : *p);
        if (has(r, "cluster")) printf(" %lld", inum(r, "cluster"));
        if (*str(r, "detail")) printf("  (%s)", str(r, "detail"));
    }
    printf("\n");
}

void human_whatfile(const cJSON *o) {
    const cJSON *parts = get(o, "partitions");
    for (const cJSON *p = parts ? parts->child : NULL; p; p = p->next) {
        const cJSON *v = get(p, "volume");
        char type[16];
        if (has(p, "type")) snprintf(type, sizeof(type), "0x%02llX", inum(p, "type"));
        else snprintf(type, sizeof(type), "-");
        printf("%-4s LBA %lld-%lld  type %s  %s", str(p, "name"), inum(p, "start_lba"), inum(p, "end_lba"), type,
               str(p, "type_name"));
        if (cJSON_IsObject(v)) {
            const char *lab = has(v, "label") && *str(v, "label") ? str(v, "label") : "no label";
            printf("  [%s ", str(v, "fs"));
            put_text(lab);
            if (has(v, "files"))
                printf(", %lld files, %lld dirs, %lld deleted entries]", inum(v, "files"), inum(v, "directories"),
                       inum(v, "deleted_entries"));
            else
                printf(", %s]", str(v, "error"));
        } else if (has(p, "fat")) {
            printf("  [%s]", str(p, "fat"));
        }
        if (*str(p, "note")) printf("  (%s)", str(p, "note"));
        printf("\n");
    }
    printf("\n");
    for (const cJSON *r = get(o, "results") ? get(o, "results")->child : NULL; r; r = r->next) human_whatfile_line(r);
    const cJSON *files = get(o, "files");
    if (cJSON_GetArraySize(files)) {
        printf("\naffected objects:\n");
        for (const cJSON *x = files->child; x; x = x->next) {
            const cJSON *lb = get(x, "lbas");
            int n = cJSON_GetArraySize(lb), i = 0;
            printf("  %-9s %s %s  %d sector%s: ", str(x, "kind"), has(x, "partition") ? str(x, "partition") : "None",
                   str(x, "path"), n, n == 1 ? "" : "s");
            for (const cJSON *y = lb ? lb->child : NULL; y && i < 8; y = y->next, i++)
                printf("%s%lld", i ? " " : "", (long long)y->valuedouble);
            printf("%s%s\n", n > 8 ? " ..." : "", tru(x, "slack_only") ? "  (slack only, data intact)" : "");
        }
    }
}
