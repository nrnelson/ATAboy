// Human-readable output of image, rescue and hashes (the Python tool's layout).

#include "cli.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Python repr() of a JSON value, as print() shows a dict or list.
static void py_repr(const cJSON *j, FILE *f) {
    if (cJSON_IsObject(j) || cJSON_IsArray(j)) {
        bool obj = cJSON_IsObject(j);
        fputc(obj ? '{' : '[', f);
        for (const cJSON *c = j->child; c; c = c->next) {
            if (c != j->child) fputs(", ", f);
            if (obj) {
                cJSON key;
                memset(&key, 0, sizeof(key));
                key.type = cJSON_String;
                key.valuestring = c->string ? c->string : (char *)"";
                py_repr(&key, f);
                fputs(": ", f);
            }
            py_repr(c, f);
        }
        fputc(obj ? '}' : ']', f);
    } else if (cJSON_IsString(j)) {
        const char *s = j->valuestring ? j->valuestring : "";
        char q = (strchr(s, '\'') && !strchr(s, '"')) ? '"' : '\'';
        fputc(q, f);
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            if (*p == '\\') fputs("\\\\", f);
            else if (*p == (unsigned char)q) { fputc('\\', f); fputc(q, f); }
            else if (*p == '\n') fputs("\\n", f);
            else if (*p == '\r') fputs("\\r", f);
            else if (*p == '\t') fputs("\\t", f);
            else if (*p < 0x20 || *p == 0x7F) fprintf(f, "\\x%02x", *p);
            else fputc(*p, f);
        }
        fputc(q, f);
    } else if (cJSON_IsNumber(j)) {
        double v = j->valuedouble;
        if (v == floor(v) && fabs(v) < 9007199254740992.0) fprintf(f, "%.0f", v);
        else {
            char t[40];
            for (int prec = 15; prec <= 17; prec++) {
                snprintf(t, sizeof(t), "%.*g", prec, v);
                if (strtod(t, NULL) == v) break;
            }
            fputs(t, f);
        }
    } else if (cJSON_IsTrue(j)) {
        fputs("True", f);
    } else if (cJSON_IsFalse(j)) {
        fputs("False", f);
    } else {
        fputs("None", f);
    }
}

void human_image(const cJSON *o) {
    const cJSON *k = ab_jobj_get(o, "kB_per_s");
    char rate[40];
    if (cJSON_IsNumber(k)) snprintf(rate, sizeof(rate), "%.1f", k->valuedouble);
    else snprintf(rate, sizeof(rate), "None");
    const char *tail = "";
    if (!ab_jbool(o, "complete", true))
        tail = strcmp(ab_jstr(o, "result", ""), "interrupted") == 0 ? " (interrupted; run again to resume)"
                                                                    : " (time budget ran out; run again to resume)";
    printf("imaged %.0f sectors in %.1f s (%s kB/s), %d bad%s; map %s\n", ab_jnum(o, "imaged_sectors", 0),
           ab_jnum(o, "seconds", 0), rate, cJSON_GetArraySize(ab_jobj_get(o, "bad_lbas")), tail,
           ab_jstr(o, "mapfile", ""));
}

void human_rescue(const cJSON *o) {
    const char *tail = "";
    if (!ab_jbool(o, "complete", true))
        tail = strcmp(ab_jstr(o, "result", ""), "interrupted") == 0 ? " (interrupted)" : " (time budget ran out)";
    printf("rescue: %.0f candidate sectors, %.0f recovered (%d fast, %d with retries), %d READ LONG, %d still bad%s\n",
           ab_jnum(o, "candidates", 0), ab_jnum(o, "recovered", 0),
           cJSON_GetArraySize(ab_jobj_get(o, "recovered_no_retry")), cJSON_GetArraySize(ab_jobj_get(o, "recovered_retry")),
           cJSON_GetArraySize(ab_jobj_get(o, "readlong")), cJSON_GetArraySize(ab_jobj_get(o, "still_bad")), tail);
    const cJSON *errs = ab_jobj_get(o, "errors");
    int ne = cJSON_GetArraySize(errs);
    if (ne) {
        printf("  %d sector(s) hit device errors: ", ne);
        cJSON *first = cJSON_CreateArray();             // errors[:5]
        int i = 0;
        for (const cJSON *x = errs->child; x && i < 5; x = x->next, i++)
            cJSON_AddItemToArray(first, cJSON_Duplicate(x, true));
        py_repr(first, stdout);
        cJSON_Delete(first);
        printf("\n");
    }
    int ns = cJSON_GetArraySize(ab_jobj_get(o, "readlong_skipped"));
    if (ns) printf("  READ LONG skipped for %d sector(s) above the 28-bit limit\n", ns);
}

void human_hashes(const cJSON *j) {
    for (const cJSON *x = ab_jobj_get(j, "records") ? ab_jobj_get(j, "records")->child : NULL; x; x = x->next)
        printf("%8.0f  lba %-10.0f %-8s %s%s\n", ab_jnum(x, "seq", 0), ab_jnum(x, "lba", 0), ab_jstr(x, "source", ""),
               ab_jstr(x, "sha256", ""), ab_jbool(x, "err_data", false) ? "  ERR-DATA" : "");
    const cJSON *c = ab_jobj_get(j, "compare");
    if (c) {
        const cJSON *d = ab_jobj_get(c, "differ");
        printf("compared with %s: %.0f match, %d differ, %.0f beyond its end\n", ab_jstr(c, "image", ""),
               ab_jnum(c, "match", 0), cJSON_GetArraySize(d), ab_jnum(c, "beyond_end", 0));
        for (const cJSON *x = d ? d->child : NULL; x; x = x->next)
            printf("  DIFFER  lba %.0f  seq %.0f\n", ab_jnum(x, "lba", 0), ab_jnum(x, "seq", 0));
    }
    char per[48] = "";
    const cJSON *u = ab_jobj_get(j, "us_per_sector");
    if (cJSON_IsNumber(u)) snprintf(per, sizeof(per), ", %.1f us per sector", u->valuedouble);
    printf("%s%d records shown; %.0f sectors hashed since boot/clear, ring holds %.0f%s\n",
           ab_jbool(j, "active", false) ? "" : "hashing is OFF (self-test failed); ",
           cJSON_GetArraySize(ab_jobj_get(j, "records")), ab_jnum(j, "newest", 0), ab_jnum(j, "capacity", 0), per);
}
