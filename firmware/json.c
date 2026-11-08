#include "json.h"
#include <string.h>

void j_init(json_t *j, char *buf, uint32_t cap) {
    memset(j, 0, sizeof(*j));
    j->buf = buf;
    j->cap = cap;
}

static void put(json_t *j, const char *s, uint32_t n) {
    if (j->overflow) return;
    if (j->len + n > j->cap) { j->overflow = true; return; }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
}
static void putc_(json_t *j, char c) { put(j, &c, 1); }
static void puts_(json_t *j, const char *s) { put(j, s, (uint32_t)strlen(s)); }

static void put_escaped(json_t *j, const char *s) {
    static const char hex[] = "0123456789abcdef";
    putc_(j, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putc_(j, '\\'); putc_(j, (char)c); }
        else if (c < 0x20 || c >= 0x7F) {
            char e[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            put(j, e, 6);
        } else putc_(j, (char)c);
    }
    putc_(j, '"');
}

static void key(json_t *j, const char *k) {
    if (j->depth && j->comma[j->depth - 1]) putc_(j, ',');
    if (j->depth) j->comma[j->depth - 1] = true;
    if (k) { put_escaped(j, k); putc_(j, ':'); }
}

static void open(json_t *j, const char *k, char o, char c) {
    key(j, k);
    putc_(j, o);
    if (j->depth < JSON_MAX_DEPTH) {
        j->comma[j->depth] = false;
        j->close[j->depth] = c;
        j->depth++;
    } else j->overflow = true;
}

void j_obj(json_t *j, const char *k) { open(j, k, '{', '}'); }
void j_arr(json_t *j, const char *k) { open(j, k, '[', ']'); }
void j_end(json_t *j) {
    if (!j->depth) return;
    j->depth--;
    putc_(j, j->close[j->depth]);
}

static void put_num(json_t *j, const char *k, bool neg, uint64_t u) {
    char tmp[21]; int n = 0;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    key(j, k);
    if (neg) putc_(j, '-');
    while (n) putc_(j, tmp[--n]);
}
void j_uint(json_t *j, const char *k, uint64_t v) { put_num(j, k, false, v); }
void j_int(json_t *j, const char *k, int64_t v) {
    put_num(j, k, v < 0, v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v);
}
void j_bool(json_t *j, const char *k, bool v) { key(j, k); puts_(j, v ? "true" : "false"); }
void j_str(json_t *j, const char *k, const char *s) { key(j, k); put_escaped(j, s ? s : ""); }
void j_null(json_t *j, const char *k) { key(j, k); puts_(j, "null"); }

json_mark_t j_mark(const json_t *j) {
    json_mark_t m = { j->len, j->depth, j->depth ? j->comma[j->depth - 1] : false };
    return m;
}
void j_restore(json_t *j, json_mark_t m) {
    j->len = m.len;
    j->depth = m.depth;
    if (m.depth) j->comma[m.depth - 1] = m.comma;
    j->overflow = false;
}

static const char *bits(uint8_t v, const char *const names[8], char *out) {
    out[0] = '\0';
    for (int b = 7; b >= 0; b--) {
        if (v & (1u << b)) {
            if (out[0]) strcat(out, "|");
            strcat(out, names[7 - b]);
        }
    }
    return out;
}

const char *ata_status_bits(uint8_t st, char *out) {
    static const char *const n[8] = {"BSY", "DRDY", "DF", "DSC", "DRQ", "CORR", "IDX", "ERR"};
    return bits(st, n, out);
}
const char *ata_error_bits(uint8_t err, char *out) {
    static const char *const n[8] = {"BBK", "UNC", "MC", "IDNF", "MCR", "ABRT", "TK0NF", "AMNF"};
    return bits(err, n, out);
}
