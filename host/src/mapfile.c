// GNU ddrescue mapfiles: load, save (atomically), mark, query.
//
// The file is untrusted input: every line is checked, numbers are parsed with
// overflow checks, and the map is kept as a sorted array of blocks (never one
// entry per sector), so a 2 TB map with millions of fragments stays cheap.
// The array is a gap buffer: the free slots sit where the last mark()
// happened, so the in-order marks of image and rescue cost O(1) each instead
// of moving the whole tail of the array.

#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_MAX_FILE   (1024u * 1024u * 1024u)   // refuse absurd inputs (1 GiB)
#define MAP_MAX_BYTES  (UINT64_C(1) << 62)        // positions beyond this are nonsense
#define BAD_MAX_SECTORS ((size_t)1 << 28)         // list at most this many sectors at once

struct ab_mapfile {
    char        *path;
    char       **header;
    size_t       nheader, capheader;
    char        *cur_line;
    ab_mapblock *b;             // blocks [0, gs) at b[0..gs), [gs, n) at the end of b[0..cap)
    size_t       n, cap, gs;
    bool         merged;            // adjacent equal blocks already merged (after a mark)
};

static char *dupn(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

static ab_mapfile *map_new(const char *path) {
    ab_mapfile *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->path = dupn(path ? path : "", path ? strlen(path) : 0);
    if (!m->path) { free(m); return NULL; }
    return m;
}

void ab_mapfile_free(ab_mapfile *m) {
    if (!m) return;
    for (size_t i = 0; i < m->nheader; i++) free(m->header[i]);
    free(m->header);
    free(m->cur_line);
    free(m->b);
    free(m->path);
    free(m);
}

static bool add_header(ab_mapfile *m, const char *s, size_t n) {
    if (m->nheader == m->capheader) {
        size_t nc = m->capheader ? m->capheader * 2 : 8;
        char **nh = realloc(m->header, nc * sizeof(*nh));
        if (!nh) return false;
        m->header = nh;
        m->capheader = nc;
    }
    char *d = dupn(s, n);
    if (!d) return false;
    m->header[m->nheader++] = d;
    return true;
}

static bool reserve_blocks(ab_mapfile *m, size_t need) {
    if (need <= m->cap) return true;
    size_t nc = m->cap ? m->cap : 16;
    while (nc < need) {
        if (nc > SIZE_MAX / 2 / sizeof(ab_mapblock)) return false;
        nc *= 2;
    }
    ab_mapblock *nb = realloc(m->b, nc * sizeof(*nb));
    if (!nb) return false;
    size_t tail = m->n - m->gs;             // keep the blocks after the gap at the end
    if (tail) memmove(&nb[nc - tail], &nb[m->cap - tail], tail * sizeof(*nb));
    m->b = nb;
    m->cap = nc;
    return true;
}

static ab_mapblock *at(const ab_mapfile *m, size_t i) { return &m->b[i < m->gs ? i : i + (m->cap - m->n)]; }

static void move_gap(ab_mapfile *m, size_t p) {
    size_t gap = m->cap - m->n;
    if (!gap) { m->gs = p; return; }
    if (p < m->gs) memmove(&m->b[p + gap], &m->b[p], (m->gs - p) * sizeof(ab_mapblock));
    else if (p > m->gs) memmove(&m->b[m->gs], &m->b[m->gs + gap], (p - m->gs) * sizeof(ab_mapblock));
    m->gs = p;
}

// Replace blocks [lo, hi) with nw[0..k).
static bool splice(ab_mapfile *m, size_t lo, size_t hi, const ab_mapblock *nw, size_t k) {
    move_gap(m, hi);
    m->gs = lo;                              // the old blocks join the gap
    m->n -= hi - lo;
    if (!reserve_blocks(m, m->n + k)) return false;
    memcpy(&m->b[m->gs], nw, k * sizeof(ab_mapblock));
    m->gs += k;
    m->n += k;
    return true;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n'; }
static bool is_status(char c) { return c == '?' || c == '*' || c == '/' || c == '-' || c == '+'; }

// Python int(s, 0) for a non-negative field: 0x.., 0o.., 0b.., or decimal
// without leading zeros.  False on anything else or overflow.
static bool parse_u64(const char *s, size_t n, uint64_t *out) {
    unsigned base = 10;
    size_t i = 0;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (n >= 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; i = 2; }
    else if (n >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; i = 2; }
    if (i >= n) return false;
    if (base == 10 && s[0] == '0') {               // "0", "00" fine; "010" is not
        for (size_t k = 0; k < n; k++) if (s[k] != '0') return false;
        *out = 0;
        return true;
    }
    uint64_t v = 0;
    for (; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return false;
        if (d >= base) return false;
        if (v > (UINT64_MAX - d) / base) return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

static int bad_line(const char *path, size_t line, const char *why) {
    return ab_fail(AB_ERR_IO, "%s:%zu: %s", path, line, why);
}

int ab_mapfile_parse(const char *text, size_t len, const char *path, ab_mapfile **out) {
    *out = NULL;
    if (!path) path = "mapfile";
    ab_mapfile *m = map_new(path);
    if (!m) return ab_fail(AB_ERR_NOMEM, "out of memory");
    size_t p = 0, lineno = 0;
    int rc = AB_OK;
    while (p < len) {
        // One line; "\n", "\r\n" and a lone "\r" end it (Python universal newlines).
        size_t s = p;
        while (p < len && text[p] != '\n' && text[p] != '\r') p++;
        size_t e = p;
        if (p < len && text[p] == '\r') p++;
        if (p < len && text[p] == '\n' && (p == e || text[p - 1] == '\r')) p++;
        lineno++;
        if (memchr(text + s, '\0', e - s)) { rc = bad_line(path, lineno, "NUL byte"); goto fail; }
        size_t a = s, z = e;
        while (a < z && is_space(text[a])) a++;
        while (z > a && is_space(text[z - 1])) z--;
        if (a == z || text[a] == '#') {
            if (!m->n && !m->cur_line && !add_header(m, text + s, e - s)) goto nomem;
            continue;
        }
        if (!m->cur_line) {
            if (!(m->cur_line = dupn(text + s, e - s))) goto nomem;
            continue;
        }
        // "pos size status": fields separated by whitespace (more are ignored)
        const char *f[3];
        size_t fl[3], nf = 0;
        for (size_t q = a; q < z && nf < 3;) {
            while (q < z && is_space(text[q])) q++;
            size_t fs = q;
            while (q < z && !is_space(text[q])) q++;
            if (q > fs) { f[nf] = text + fs; fl[nf] = q - fs; nf++; }
        }
        if (nf < 3) { rc = bad_line(path, lineno, "expected 'pos size status'"); goto fail; }
        uint64_t pos, size;
        if (!parse_u64(f[0], fl[0], &pos)) { rc = bad_line(path, lineno, "bad position"); goto fail; }
        if (!parse_u64(f[1], fl[1], &size)) { rc = bad_line(path, lineno, "bad size"); goto fail; }
        if (fl[2] != 1 || !is_status(f[2][0])) { rc = bad_line(path, lineno, "bad status (not one of ?*/-+)"); goto fail; }
        if (size == 0) { rc = bad_line(path, lineno, "block of size 0"); goto fail; }
        if (pos > MAP_MAX_BYTES || size > MAP_MAX_BYTES - pos) { rc = bad_line(path, lineno, "block beyond 2^62 bytes"); goto fail; }
        if (m->n && pos < m->b[m->n - 1].pos + m->b[m->n - 1].size) {   // (gap is at the end while parsing)
            rc = bad_line(path, lineno, "blocks overlap or are out of order");
            goto fail;
        }
        if (!reserve_blocks(m, m->n + 1)) goto nomem;
        m->b[m->n].pos = pos;
        m->b[m->n].size = size;
        m->b[m->n].status = f[2][0];
        m->n++;
        m->gs = m->n;
    }
    if (!m->cur_line) { rc = ab_fail(AB_ERR_IO, "%s: not a ddrescue mapfile", path); goto fail; }
    *out = m;
    return AB_OK;
nomem:
    rc = ab_fail(AB_ERR_NOMEM, "out of memory reading %s", path);
fail:
    ab_mapfile_free(m);
    return rc;
}

int ab_mapfile_load(const char *path, ab_mapfile **out) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return ab_fail(AB_ERR_IO, "cannot open %s: %s", path, strerror(errno));
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= MAP_MAX_FILE) { free(buf); fclose(f); return ab_fail(AB_ERR_IO, "%s: too large for a mapfile", path); }
            size_t nc = cap ? cap * 2 : 65536;
            char *nb = realloc(buf, nc);
            if (!nb) { free(buf); fclose(f); return ab_fail(AB_ERR_NOMEM, "out of memory reading %s", path); }
            buf = nb;
            cap = nc;
        }
        size_t k = fread(buf + len, 1, cap - len, f);
        len += k;
        if (k == 0) break;
    }
    bool err = ferror(f) != 0;
    fclose(f);
    if (err) { free(buf); return ab_fail(AB_ERR_IO, "cannot read %s", path); }
    int rc = ab_mapfile_parse(buf ? buf : "", len, path, out);
    free(buf);
    return rc;
}

int ab_mapfile_create(const char *path, uint64_t size_bytes, ab_mapfile **out) {
    *out = NULL;
    if (size_bytes > MAP_MAX_BYTES) return ab_fail(AB_ERR_ARG, "device too large for a mapfile");
    ab_mapfile *m = map_new(path);
    if (!m) return ab_fail(AB_ERR_NOMEM, "out of memory");
    static const char hdr[] = "# Mapfile. Created by ataboy " ATABOY_LIB_VERSION;
    static const char cur[] = "0x00000000     ?               1";
    if (!add_header(m, hdr, strlen(hdr)) || !(m->cur_line = dupn(cur, strlen(cur)))) {
        ab_mapfile_free(m);
        return ab_fail(AB_ERR_NOMEM, "out of memory");
    }
    if (size_bytes) {
        if (!reserve_blocks(m, 1)) { ab_mapfile_free(m); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
        m->b[0].pos = 0;
        m->b[0].size = size_bytes;
        m->b[0].status = '?';
        m->n = 1;
        m->gs = 1;
    }
    *out = m;
    return AB_OK;
}

int ab_mapfile_save(ab_mapfile *m) {
    size_t pl = strlen(m->path);
    char *tmp = malloc(pl + 5);
    if (!tmp) return ab_fail(AB_ERR_NOMEM, "out of memory");
    memcpy(tmp, m->path, pl);
    memcpy(tmp + pl, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        int rc = ab_fail(AB_ERR_IO, "cannot write %s: %s", tmp, strerror(errno));
        free(tmp);
        return rc;
    }
    bool ok = true;
    for (size_t i = 0; i < m->nheader && ok; i++) ok = fprintf(f, "%s\n", m->header[i]) >= 0;
    ok = ok && fprintf(f, "%s\n", m->cur_line) >= 0;
    ok = ok && fputs("#      pos        size  status\n", f) >= 0;
    for (size_t i = 0; i < m->n && ok; i++) {
        const ab_mapblock *b = at(m, i);
        ok = fprintf(f, "0x%08llX  0x%08llX  %c\n", (unsigned long long)b->pos, (unsigned long long)b->size,
                     b->status) >= 0;
    }
    int rc = ok ? ab_file_sync(f, tmp) : ab_fail(AB_ERR_IO, "cannot write %s: %s", tmp, strerror(errno));
    if (fclose(f) != 0 && rc == AB_OK) rc = ab_fail(AB_ERR_IO, "cannot write %s: %s", tmp, strerror(errno));
    if (rc == AB_OK) rc = ab_file_replace(tmp, m->path);
    if (rc != AB_OK) remove(tmp);
    else ab_give_back(m->path);
    free(tmp);
    return rc;
}

// First block whose end is beyond byte a.
static size_t first_ending_after(const ab_mapfile *m, uint64_t a) {
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const ab_mapblock *b = at(m, mid);
        if (b->pos + b->size <= a) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// Merge equal, contiguous neighbours over the whole map (contiguous array).
static void merge_all(ab_mapfile *m) {
    move_gap(m, m->n);
    if (!m->n) return;
    size_t w = 0;
    for (size_t r = 1; r < m->n; r++) {
        ab_mapblock *last = &m->b[w];
        if (last->status == m->b[r].status && last->pos + last->size == m->b[r].pos) last->size += m->b[r].size;
        else m->b[++w] = m->b[r];
    }
    m->n = w + 1;
    m->gs = m->n;
}

static void push(ab_mapblock *nw, size_t *k, ab_mapblock x) {
    if (*k && nw[*k - 1].status == x.status && nw[*k - 1].pos + nw[*k - 1].size == x.pos) nw[*k - 1].size += x.size;
    else nw[(*k)++] = x;
}

int ab_mapfile_mark(ab_mapfile *m, uint64_t lba, uint64_t n, char status) {
    if (!is_status(status)) return ab_fail(AB_ERR_ARG, "bad mapfile status '%c'", status);
    if (lba > MAP_MAX_BYTES / 512 || n > MAP_MAX_BYTES / 512 - lba) return ab_fail(AB_ERR_ARG, "sector beyond the map's range");
    if (!m->merged) {           // Python's mark() merges the whole map every time
        merge_all(m);
        m->merged = true;
    }
    uint64_t a = lba * 512, b = (lba + n) * 512;
    if (a >= b) return AB_OK;
    size_t i0 = first_ending_after(m, a), i1 = i0;
    while (i1 < m->n && at(m, i1)->pos < b) i1++;
    if (i0 == i1) return AB_OK;
    // Replacement for [lo, hi) = [i0, i1) plus one neighbour each side (the
    // map is merged, so only they can merge with the new parts): neighbour,
    // head remainder, the relabelled parts, tail remainder, neighbour.
    size_t lo = i0 ? i0 - 1 : 0, hi = i1 < m->n ? i1 + 1 : i1;
    ab_mapblock small[8], *nw = small;
    size_t maxnew = (hi - lo) + 2;
    if (maxnew > 8 && !(nw = malloc(maxnew * sizeof(*nw)))) return ab_fail(AB_ERR_NOMEM, "out of memory");
    size_t k = 0;
    ab_mapblock first = *at(m, i0), last = *at(m, i1 - 1);
    if (lo < i0) push(nw, &k, *at(m, lo));
    if (first.pos < a) push(nw, &k, (ab_mapblock){first.pos, a - first.pos, first.status});
    for (size_t i = i0; i < i1; i++) {
        const ab_mapblock *x = at(m, i);
        uint64_t s0 = x->pos > a ? x->pos : a;
        uint64_t end = x->pos + x->size;
        uint64_t s1 = end < b ? end : b;
        push(nw, &k, (ab_mapblock){s0, s1 - s0, status});
    }
    if (last.pos + last.size > b) push(nw, &k, (ab_mapblock){b, last.pos + last.size - b, last.status});
    if (hi > i1) push(nw, &k, *at(m, i1));
    bool ok = splice(m, lo, hi, nw, k);
    if (nw != small) free(nw);
    return ok ? AB_OK : ab_fail(AB_ERR_NOMEM, "out of memory");
}

char ab_mapfile_status_at(const ab_mapfile *m, uint64_t lba) {
    if (lba > MAP_MAX_BYTES / 512) return '\0';
    uint64_t a = lba * 512;
    size_t i = first_ending_after(m, a);
    if (i < m->n && at(m, i)->pos <= a) return at(m, i)->status;
    return '\0';
}

static bool wanted(char st, bool include_untried) { return st != '+' && (st != '?' || include_untried); }

int ab_mapfile_bad_sectors(const ab_mapfile *m, uint64_t limit, bool include_untried, uint64_t **lbas, size_t *n) {
    *lbas = NULL;
    *n = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < m->n; i++) {
        const ab_mapblock *b = at(m, i);
        if (!wanted(b->status, include_untried)) continue;
        total += (b->pos + b->size + 511) / 512 - b->pos / 512;
        if (limit && total >= limit) { total = limit; break; }
    }
    if (total > BAD_MAX_SECTORS)
        return ab_fail(AB_ERR_NOMEM, "%llu sectors to list: too many at once (use a sector limit)",
                       (unsigned long long)total);
    if (!total) return AB_OK;
    uint64_t *v = malloc((size_t)total * sizeof(*v));
    if (!v) return ab_fail(AB_ERR_NOMEM, "out of memory");
    size_t k = 0;
    for (size_t i = 0; i < m->n && k < total; i++) {
        const ab_mapblock *b = at(m, i);
        if (!wanted(b->status, include_untried)) continue;
        uint64_t end = (b->pos + b->size + 511) / 512;
        for (uint64_t s = b->pos / 512; s < end && k < total; s++) v[k++] = s;
    }
    *lbas = v;
    *n = k;
    return AB_OK;
}

cJSON *ab_mapfile_summary(const ab_mapfile *m) {
    // Keys in order of first appearance (a Python dict).
    char order[8];
    uint64_t tot[8] = {0};
    int no = 0;
    for (size_t i = 0; i < m->n; i++) {
        const ab_mapblock *b = at(m, i);
        int k = 0;
        while (k < no && order[k] != b->status) k++;
        if (k == no) order[no++] = b->status;
        tot[k] += b->size;
    }
    cJSON *o = cJSON_CreateObject();
    for (int k = 0; k < no; k++) {
        char key[2] = {order[k], '\0'};
        cJSON_AddNumberToObject(o, key, (double)tot[k]);
    }
    return o;
}

const ab_mapblock *ab_mapfile_blocks(const ab_mapfile *m, size_t *n) {
    move_gap((ab_mapfile *)m, m->n);     // logically const: only closes the gap
    *n = m->n;
    return m->b;
}
const char *ab_mapfile_path(const ab_mapfile *m) { return m->path; }
size_t ab_mapfile_header_lines(const ab_mapfile *m) { return m->nheader; }
const char *ab_mapfile_header_line(const ab_mapfile *m, size_t i) { return i < m->nheader ? m->header[i] : NULL; }
const char *ab_mapfile_cur_line(const ab_mapfile *m) { return m->cur_line; }
