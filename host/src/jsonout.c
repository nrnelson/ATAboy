// JSON text output in the layout of Python's json.dumps(obj, indent=N) (and
// indent=None: one line with ", " / ": " separators), ASCII-only like its
// default ensure_ascii, so the CLI's output matches the old Python tool.

#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *s; size_t len, cap; bool oom; } sbuf;

static void put(sbuf *b, const char *p, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *ns = realloc(b->s, nc);
        if (!ns) { b->oom = true; return; }
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->len, p, n);
    b->len += n;
    b->s[b->len] = '\0';
}
static void puts_(sbuf *b, const char *p) { put(b, p, strlen(p)); }

static void newline(sbuf *b, int indent, int level) {
    put(b, "\n", 1);
    for (int i = 0; i < indent * level; i++) put(b, " ", 1);
}

static void put_u16(sbuf *b, unsigned v) {
    char t[8];
    snprintf(t, sizeof(t), "\\u%04x", v & 0xFFFF);
    puts_(b, t);
}

// Decode one UTF-8 sequence.  Anything that isn't well-formed UTF-8 (bad
// lead or continuation byte, overlong form, surrogate, beyond U+10FFFF)
// comes out as U+FFFD for its first byte, like Python's errors="replace".
static unsigned utf8(const unsigned char **pp) {
    static const unsigned min[4] = {0, 0x80, 0x800, 0x10000};
    const unsigned char *p = *pp;
    unsigned c = *p++;
    *pp = p;
    if (c < 0x80) return c;
    int n = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
    if (n < 0) return 0xFFFD;
    unsigned v = c & (0x3Fu >> n);
    for (int i = 0; i < n; i++) {               // stops at the terminating NUL
        if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min[n] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0xFFFD;
    *pp = p + n;
    return v;
}

static void put_string(sbuf *b, const char *s) {
    put(b, "\"", 1);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c == '"') { puts_(b, "\\\""); p++; }
        else if (c == '\\') { puts_(b, "\\\\"); p++; }
        else if (c == '\n') { puts_(b, "\\n"); p++; }
        else if (c == '\r') { puts_(b, "\\r"); p++; }
        else if (c == '\t') { puts_(b, "\\t"); p++; }
        else if (c == '\b') { puts_(b, "\\b"); p++; }
        else if (c == '\f') { puts_(b, "\\f"); p++; }
        else if (c < 0x20 || c == 0x7F) { put_u16(b, c); p++; }      // Python escapes DEL too
        else if (c < 0x80) { put(b, (const char *)p, 1); p++; }
        else {
            unsigned v = utf8(&p);
            if (v >= 0x10000) {
                v -= 0x10000;
                put_u16(b, 0xD800 + (v >> 10));
                put_u16(b, 0xDC00 + (v & 0x3FF));
            } else {
                put_u16(b, v);
            }
        }
    }
    put(b, "\"", 1);
}

static void put_number(sbuf *b, double v) {
    char t[40];
    if (isnan(v) || isinf(v)) { puts_(b, isnan(v) ? "NaN" : v > 0 ? "Infinity" : "-Infinity"); return; }
    if (v == floor(v) && fabs(v) < 9007199254740992.0) {      // integral: as an integer
        snprintf(t, sizeof(t), "%.0f", v);
        if (strcmp(t, "-0") == 0) strcpy(t, "0");
        puts_(b, t);
        return;
    }
    for (int prec = 15; prec <= 17; prec++) {                  // shortest that round-trips
        snprintf(t, sizeof(t), "%.*g", prec, v);
        if (strtod(t, NULL) == v) break;
    }
    puts_(b, t);
}

static void emit(sbuf *b, const cJSON *j, int indent, int level) {
    if (cJSON_IsObject(j) || cJSON_IsArray(j)) {
        bool obj = cJSON_IsObject(j);
        const cJSON *c = j->child;
        if (!c) { puts_(b, obj ? "{}" : "[]"); return; }
        put(b, obj ? "{" : "[", 1);
        bool first = true;
        for (; c; c = c->next) {
            if (!first) puts_(b, indent ? "," : ", ");
            first = false;
            if (indent) newline(b, indent, level + 1);
            if (obj) { put_string(b, c->string ? c->string : ""); puts_(b, ": "); }
            emit(b, c, indent, level + 1);
        }
        if (indent) newline(b, indent, level);
        put(b, obj ? "}" : "]", 1);
    } else if (cJSON_IsString(j)) {
        put_string(b, j->valuestring ? j->valuestring : "");
    } else if (cJSON_IsNumber(j)) {
        put_number(b, j->valuedouble);
    } else if (cJSON_IsTrue(j)) {
        puts_(b, "true");
    } else if (cJSON_IsFalse(j)) {
        puts_(b, "false");
    } else if (cJSON_IsRaw(j)) {
        puts_(b, j->valuestring ? j->valuestring : "null");
    } else {
        puts_(b, "null");
    }
}

char *ab_json_print(const cJSON *j, int indent) {
    sbuf b = {0};
    emit(&b, j, indent < 0 ? 0 : indent, 0);
    if (b.oom) { free(b.s); return NULL; }
    if (!b.s) { b.s = malloc(1); if (b.s) b.s[0] = '\0'; }
    return b.s;
}
