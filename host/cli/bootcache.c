// Last boot id seen per ATAboy, kept between runs in the user's cache
// directory (Linux/macOS: $XDG_CACHE_HOME or ~/.cache; Windows:
// %LOCALAPPDATA%), so a reboot between two commands is reported.

#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_(p) mkdir(p, 0700)
#endif

// Under sudo, use (and keep owned by) the invoking user's cache, so root
// runs and normal runs see the same boot ids.
static void own(const char *path) {
#if !defined(_WIN32)
    const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    if (geteuid() == 0 && u && g && chown(path, (uid_t)atol(u), (gid_t)atol(g)) != 0) { /* best effort */ }
#else
    (void)path;
#endif
}

static int cache_path(char *out, size_t n, bool create) {
    const char *base;
    char tmp[1024];
#if defined(_WIN32)
    base = getenv("LOCALAPPDATA");
    if (!base) return -1;
    snprintf(tmp, sizeof(tmp), "%s\\ataboy", base);
    if (create) mkdir_(tmp);
    snprintf(out, n, "%s\\boot-ids.json", tmp);
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *su = getenv("SUDO_USER");
    struct passwd *pw = (geteuid() == 0 && su && *su) ? getpwnam(su) : NULL;
    if (pw && pw->pw_dir) snprintf(tmp, sizeof(tmp), "%s/.cache", pw->pw_dir);
    else if (xdg && *xdg) snprintf(tmp, sizeof(tmp), "%s", xdg);
    else if ((base = getenv("HOME")) != NULL) snprintf(tmp, sizeof(tmp), "%s/.cache", base);
    else return -1;
    if (create && mkdir_(tmp) == 0) own(tmp);
    size_t l = strlen(tmp);
    snprintf(tmp + l, sizeof(tmp) - l, "/ataboy");
    if (create && mkdir_(tmp) == 0) own(tmp);
    snprintf(out, n, "%s/boot-ids.json", tmp);
#endif
    return 0;
}

static cJSON *load(void) {
    char p[1100];
    if (cache_path(p, sizeof(p), false) != 0) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    return j;
}

void bootcache_seed(ab_dev *dev) {
    cJSON *j = load();
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, ab_dev_key(dev));
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 4294967295.0)
        ab_seed_boot_id(dev, (uint32_t)v->valuedouble);
    cJSON_Delete(j);
}

void bootcache_save(ab_dev *dev) {
    uint32_t id;
    if (!ab_boot_id(dev, &id)) return;
    cJSON *j = load();
    if (!j) j = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(j, ab_dev_key(dev));
    cJSON_AddNumberToObject(j, ab_dev_key(dev), id);
    char p[1100], t[1110];
    char *s = ab_json_print(j, 0);
    cJSON_Delete(j);
    if (!s || cache_path(p, sizeof(p), true) != 0) { free(s); return; }
    snprintf(t, sizeof(t), "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (f) {
        fputs(s, f);
        if (fclose(f) == 0) {
#if defined(_WIN32)
            remove(p);
#endif
            if (rename(t, p) == 0) own(p);
        }
    }
    free(s);
}
