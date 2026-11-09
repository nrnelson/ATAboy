// whatfile: map 512-byte image LBAs to whatever owns them in a FAT12/16/32
// file system (MBR primary and logical partitions, or a bare volume): files
// with their byte offset, directories with the entries in that sector, FAT
// copies, boot/reserved sectors, root directory, free/lost/bad clusters and
// deleted entries (best effort).  Same JSON as the Python tool's whatfile().
//
// The input is an image of a damaged disk, so every on-disk value is
// untrusted: reads past the end of the image return zeros (like Python's
// _read_at), cluster chains stop at loops, and the index has fixed limits
// (see below).  When a limit cuts the index short, the volume's info says
// so ("index_truncated"); Python had no limits and would just run out of
// memory or time on such images.

#include "internal.h"
#include "fat_tables.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512

// Index limits (per volume).  Real FAT volumes stay far below all of them.
#define FAT_MAX_ENTRIES  (1u << 26)       // decoded FAT entries held in memory
#define MAX_OBJECTS      (1u << 21)       // files + directories + deleted entries
#define MAX_DIR_SLOTS    65536u           // 32-byte slots per directory (the FAT maximum)
#define MAX_DIR_BYTES    (1ull << 30)     // directory data parsed in total
#define MAX_CHAIN_STEPS  (1ull << 28)     // FAT entries followed in total
#define MAX_POOL_BYTES   (1u << 27)       // names
#define MAX_DEPTH        128              // directory nesting
#define MAX_LFN          20               // long-name slots per entry (255 characters)

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
//  Image source: a file or a memory buffer; zeros past the end
// ---------------------------------------------------------------------------

typedef struct {
    FILE          *f;
    const uint8_t *mem;
    uint64_t       size;
    bool           err;
} src_t;

static void src_read(src_t *s, uint64_t off, uint8_t *buf, size_t n) {
    memset(buf, 0, n);
    if (off >= s->size) return;
    uint64_t avail = s->size - off;
    size_t m = avail < n ? (size_t)avail : n;
    if (s->mem) {
        memcpy(buf, s->mem + off, m);
        return;
    }
    if (ab_fseek(s->f, (long long)off, SEEK_SET) != 0) {
        s->err = true;
        return;
    }
    if (fread(buf, 1, m, s->f) < m && ferror(s->f)) {
        s->err = true;
        clearerr(s->f);
    }
}

// ---------------------------------------------------------------------------
//  Text: cp437 / UTF-16 to UTF-8, as Python decoded them
// ---------------------------------------------------------------------------

typedef struct {
    char   s[1280];          // 20 LFN slots * 13 units * 4 bytes + short name
    size_t len;
} ustr;

// U+0000 (NUL bytes in labels and OEM names) is stored as the two bytes
// C0 80, so it survives in C strings; the JSON writer turns it into \u0000
// and the CLI's human output back into a NUL byte, as Python printed it.
static void u_put(ustr *u, uint32_t c) {
    char t[4];
    size_t n;
    if (c == 0) { t[0] = (char)0xC0; t[1] = (char)0x80; n = 2; }
    else if (c < 0x80) { t[0] = (char)c; n = 1; }
    else if (c < 0x800) { t[0] = (char)(0xC0 | (c >> 6)); t[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        t[0] = (char)(0xE0 | (c >> 12)); t[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[2] = (char)(0x80 | (c & 0x3F)); n = 3;
    } else {
        t[0] = (char)(0xF0 | (c >> 18)); t[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((c >> 6) & 0x3F)); t[3] = (char)(0x80 | (c & 0x3F)); n = 4;
    }
    if (u->len + n >= sizeof(u->s)) return;
    memcpy(u->s + u->len, t, n);
    u->len += n;
    u->s[u->len] = '\0';
}

static void u_init(ustr *u) { u->len = 0; u->s[0] = '\0'; }

// str.isspace() for a cp437 byte's character (what str.rstrip() removes)
static bool cp437_space(uint8_t b) { return (b >= 0x09 && b <= 0x0D) || (b >= 0x1C && b <= 0x20) || b == 0xFF; }

// bytes.decode("cp437").rstrip() into out (UTF-8)
static void cp437_rstrip(const uint8_t *p, size_t n, char *out, size_t cap) {
    while (n && cp437_space(p[n - 1])) n--;
    ustr u;
    u_init(&u);
    for (size_t i = 0; i < n; i++) u_put(&u, cp437_ucs[p[i]]);
    snprintf(out, cap, "%s", u.s);
}

// The 13 UTF-16 units of a long-name slot, decoded with errors="replace",
// cut at the first NUL, U+FFFF padding removed.  Returns the code points.
static size_t lfn_chars(const uint8_t *e, uint32_t out[13]) {
    uint16_t w[13];
    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int i = 0; i < 13; i++) w[i] = le16(e + at[i]);
    uint32_t cps[13];
    size_t n = 0;
    for (int i = 0; i < 13; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 < 13 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c < 0xE000) {
            c = 0xFFFD;
        }
        cps[n++] = c;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (cps[i] == 0) break;
        if (cps[i] != 0xFFFF) out[k++] = cps[i];
    }
    return k;
}

// c.upper()[:1].encode("cp437", "replace")[:1]
static uint8_t upper_cp437_of(uint32_t c) {
    size_t lo = 0, hi = sizeof(upper_cp437) / sizeof(upper_cp437[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (upper_cp437[mid].ucs == c) return upper_cp437[mid].cp;
        if (upper_cp437[mid].ucs < c) lo = mid + 1;
        else hi = mid;
    }
    return '?';
}

static uint8_t sfn_checksum(const uint8_t *raw11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)((((s & 1) << 7) + (s >> 1) + raw11[i]) & 0xFF);
    return s;
}

static bool fat_time(uint16_t d, uint16_t t, char *out, size_t cap) {
    int y = 1980 + (d >> 9), mo = (d >> 5) & 15, dd = d & 31, h = t >> 11, mi = (t >> 5) & 63, sec = (t & 31) * 2;
    if (!(mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && h < 24 && mi < 60 && sec < 60)) return false;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, dd, h, mi, sec);
    return true;
}

// Reject stale garbage in directory slots (control or forbidden characters
// in the 8.3 name, reserved attribute bits).
static bool plausible_sfn(const uint8_t *e) {
    if ((e[11] & 0xC0) || (e[12] & 0xE7) || e[0] == 0x20 || (e[0] < 0x20 && e[0] != 0x05)) return false;
    if (e[0] == 0x2E) return !memcmp(e, ".          ", 11) || !memcmp(e, "..         ", 11);
    for (int i = 1; i < 11; i++)
        if (e[i] < 0x20 || strchr("\"*+,./:;<=>?[\\]|", e[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  Boot sector
// ---------------------------------------------------------------------------

typedef struct {
    char     oem[40];
    unsigned bps, spc, rsv, nfats, rootent, media;
    uint32_t fatsz, total, root_secs;
    uint64_t data_start, clusters;
    int      fat_type;
    uint32_t root_cluster;
    unsigned fsinfo, backup, ext_flags;
    bool     has_label;
    char     label[48];
} bpb_t;

// FAT type follows Microsoft's rule: by cluster count, not by any label.
static bool parse_bpb(const uint8_t *bs, bpb_t *b) {
    memset(b, 0, sizeof(*b));
    if (bs[0] != 0xEB && bs[0] != 0xE9) return false;
    unsigned bps = le16(bs + 11), spc = bs[13], rsv = le16(bs + 14), nfats = bs[16], rootent = le16(bs + 17);
    unsigned tot16 = le16(bs + 19), media = bs[21], fat16 = le16(bs + 22);
    uint32_t tot32 = le32(bs + 32), fat32sz = le32(bs + 36);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || spc == 0 || (spc & (spc - 1)) || rsv == 0 ||
        nfats < 1 || nfats > 4 || !(media == 0xF0 || media >= 0xF8))
        return false;
    uint32_t fatsz = fat16 ? fat16 : fat32sz, total = tot16 ? tot16 : tot32;
    uint32_t root_secs = (rootent * 32 + bps - 1) / bps;
    uint64_t data_start = rsv + (uint64_t)nfats * fatsz + root_secs;
    if (fatsz == 0 || total <= data_start) return false;
    uint64_t clusters = (total - data_start) / spc;
    b->fat_type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    cp437_rstrip(bs + 3, 8, b->oem, sizeof(b->oem));
    b->bps = bps; b->spc = spc; b->rsv = rsv; b->nfats = nfats; b->rootent = rootent; b->media = media;
    b->fatsz = fatsz; b->total = total; b->root_secs = root_secs; b->data_start = data_start;
    b->clusters = clusters;
    const uint8_t *lab;
    uint8_t sig;
    if (b->fat_type == 32) {
        if (fat16 || rootent) return false;     // FAT32 needs BPB_FATSz16 = 0 and no fixed root
        b->ext_flags = le16(bs + 40);
        b->root_cluster = le32(bs + 44);
        b->fsinfo = le16(bs + 48);
        b->backup = le16(bs + 50);
        sig = bs[66];
        lab = bs + 71;
    } else {
        sig = bs[38];
        lab = bs + 43;
    }
    if (sig == 0x29) {
        cp437_rstrip(lab, 11, b->label, sizeof(b->label));
        b->has_label = b->label[0] != '\0';
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Small hash map: cluster -> (owner, position)
// ---------------------------------------------------------------------------

typedef struct { uint32_t key; int32_t owner; uint32_t pos; } hent;
typedef struct { hent *t; size_t cap, n; } hmap;

static hent *hmap_get(hmap *m, uint32_t key, bool insert, bool *oom) {
    if (key == 0) return NULL;                   // 0 marks a free slot; clusters start at 2
    if (insert && (m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 64;
        hent *nt = calloc(nc, sizeof(hent));
        if (!nt) { *oom = true; return NULL; }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->t[i].key) continue;
            size_t j = (m->t[i].key * 0x9E3779B1u) & (nc - 1);
            while (nt[j].key) j = (j + 1) & (nc - 1);
            nt[j] = m->t[i];
        }
        free(m->t);
        m->t = nt;
        m->cap = nc;
    }
    if (!m->cap) return NULL;
    size_t j = (key * 0x9E3779B1u) & (m->cap - 1);
    while (m->t[j].key) {
        if (m->t[j].key == key) return &m->t[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!insert) return NULL;
    m->t[j].key = key;
    m->t[j].owner = -1;
    m->t[j].pos = 0;
    m->n++;
    return &m->t[j];
}

// ---------------------------------------------------------------------------
//  Volume and index
// ---------------------------------------------------------------------------

enum { K_FILE, K_DIR };
#define NONE UINT32_MAX

typedef struct {
    uint32_t parent, name, short_name;           // names: offsets into the pool
    uint32_t first_cluster, size;
    uint32_t ent_start, ent_n;
    uint16_t mdate, mtime, depth;
    uint8_t  kind;
    bool     deleted, has_size, has_mtime;
} obj_t;

typedef struct { uint32_t offset, obj, label; } ent_t;   // label: pool offset (obj = NONE)
typedef struct { uint32_t fc, n, obj; } del_t;
typedef struct { uint32_t obj, fc; uint8_t mode; bool gone; } job_t;
enum { DIR_ROOT, DIR_CHAIN, DIR_ONE };

typedef struct vol {
    src_t   *src;
    uint64_t start_lba;
    bpb_t    b;
    int      fat_type;
    unsigned bps, spc;
    uint32_t cluster_bytes;
    uint64_t root_start, data_start, total_sectors, max_cluster;
    unsigned active_fat;
    bool     fat_mirrored;
    uint32_t bad_mark;

    uint32_t *fat;            // [fat_len]; entries past it are 0
    uint64_t  fat_len;
    int32_t  *owner;          // [fat_len]
    uint32_t *pos;
    uint8_t  *seen;           // bitmap [fat_len], clear between chain walks
    hmap      extra;          // owner/pos of clusters >= fat_len
    hmap      visited;        // directory first clusters already walked
    uint32_t *chain;
    size_t    chain_n, chain_cap;
    uint8_t  *clbuf;

    obj_t  *objs; size_t nobj, objcap;
    ent_t  *ents; size_t nent, entcap;
    del_t  *dels; size_t ndel, delcap;
    uint32_t *del_order;      // dels sorted by first cluster (built on first use)
    uint64_t *del_maxend;     // max(fc + n) over del_order[0..i]
    job_t  *jobs; size_t njob, jobcap;
    char   *pool; size_t pool_len, pool_cap;

    uint64_t    cross_links, steps, dir_bytes;
    bool        indexed, oom, stop;   // stop: a limit ended the walk
    const char *trunc;        // limit that cut the index short
    char        error[160];   // the volume could not be indexed at all
} vol_t;

static bool grow(void **p, size_t *cap, size_t need, size_t elem, bool *oom) {
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    void *np = realloc(*p, nc * elem);
    if (!np) { *oom = true; return false; }
    *p = np;
    *cap = nc;
    return true;
}

// A limit was reached: note it (the first one is reported); stop the walk if fatal.
static void limit(vol_t *v, const char *what, bool fatal) {
    if (!v->trunc) v->trunc = what;
    if (fatal) v->stop = true;
}

static uint32_t pool_add(vol_t *v, const char *s) {
    size_t n = strlen(s) + 1;
    if (v->pool_len + n > MAX_POOL_BYTES) { limit(v, "name storage", true); return NONE; }
    if (!grow((void **)&v->pool, &v->pool_cap, v->pool_len + n, 1, &v->oom)) return NONE;
    memcpy(v->pool + v->pool_len, s, n);
    uint32_t off = (uint32_t)v->pool_len;
    v->pool_len += n;
    return off;
}
static const char *pool_str(const vol_t *v, uint32_t off) { return off == NONE ? "" : v->pool + off; }

static bool vol_open(vol_t *v, src_t *src, uint64_t start_lba) {
    memset(v, 0, sizeof(*v));
    uint8_t bs[SECTOR];
    src_read(src, start_lba * SECTOR, bs, SECTOR);
    if (!parse_bpb(bs, &v->b)) return false;
    v->src = src;
    v->start_lba = start_lba;
    v->fat_type = v->b.fat_type;
    v->bps = v->b.bps;
    v->spc = v->b.spc;
    v->cluster_bytes = v->bps * v->spc;
    v->root_start = v->b.rsv + (uint64_t)v->b.nfats * v->b.fatsz;
    v->data_start = v->b.data_start;
    v->total_sectors = v->b.total;
    v->max_cluster = v->b.clusters + 1;
    v->fat_mirrored = !(v->fat_type == 32 && (v->b.ext_flags & 0x80));
    unsigned af = v->b.ext_flags & 0x0F;
    v->active_fat = v->fat_mirrored ? 0 : (af < v->b.nfats - 1 ? af : v->b.nfats - 1);
    v->bad_mark = v->fat_type == 12 ? 0xFF7 : v->fat_type == 16 ? 0xFFF7 : 0x0FFFFFF7;
    return true;
}

static void vol_free(vol_t *v) {
    free(v->fat); free(v->owner); free(v->pos); free(v->seen);
    free(v->extra.t); free(v->visited.t); free(v->chain); free(v->clbuf);
    free(v->objs); free(v->ents); free(v->dels); free(v->jobs); free(v->pool);
    free(v->del_order); free(v->del_maxend);
}

// First image LBA after the file system.
static uint64_t vol_end_lba(const vol_t *v) {
    return v->start_lba + (v->total_sectors * v->bps + SECTOR - 1) / SECTOR;
}

static void read_sectors(vol_t *v, uint64_t sector, uint64_t count, uint8_t *buf) {
    src_read(v->src, v->start_lba * SECTOR + sector * v->bps, buf, (size_t)(count * v->bps));
}

static uint32_t fat_get(const vol_t *v, uint64_t c) { return c < v->fat_len ? v->fat[c] : 0; }

static bool load_fat(vol_t *v) {
    uint64_t n = v->max_cluster + 1;
    uint64_t raw_len = (uint64_t)v->b.fatsz * v->bps;
    uint64_t off = v->start_lba * SECTOR + (v->b.rsv + (uint64_t)v->active_fat * v->b.fatsz) * v->bps;
    uint64_t avail = off < v->src->size ? v->src->size - off : 0;
    if (avail > raw_len) avail = raw_len;
    // Only the entries that can be non-zero are stored: past the image's end
    // (or the FAT's) Python padded with zeros.
    uint64_t len;
    if (v->fat_type == 12) len = avail * 2 / 3 + 2;
    else if (v->fat_type == 16) len = (avail + 1) / 2 < raw_len / 2 ? (avail + 1) / 2 : raw_len / 2;
    else len = (avail + 3) / 4 < raw_len / 4 ? (avail + 3) / 4 : raw_len / 4;
    if (len > n) len = n;
    if (len > FAT_MAX_ENTRIES) {
        snprintf(v->error, sizeof(v->error), "FAT too large to index (%llu entries, limit %u)",
                 (unsigned long long)len, FAT_MAX_ENTRIES);
        return false;
    }
    size_t L = (size_t)len;
    v->fat_len = len;
    v->fat = calloc(L ? L : 1, sizeof(uint32_t));
    v->owner = malloc((L ? L : 1) * sizeof(int32_t));
    v->pos = calloc(L ? L : 1, sizeof(uint32_t));
    v->seen = calloc(L / 8 + 1, 1);
    v->clbuf = malloc(v->cluster_bytes);
    if (!v->fat || !v->owner || !v->pos || !v->seen || !v->clbuf) { v->oom = true; return false; }
    for (size_t i = 0; i < L; i++) v->owner[i] = -1;
    if (v->fat_type == 12) {
        size_t nb = L + L / 2 + 2;              // < 6.2 KB: FAT12 has < 4087 entries
        uint8_t *raw = malloc(nb);
        if (!raw) { v->oom = true; return false; }
        src_read(v->src, off, raw, nb);
        for (size_t i = (size_t)avail; i < nb; i++) raw[i] = 0;
        for (size_t c = 0; c < L; c++) {
            size_t o = c + (c >> 1);
            uint32_t x = o + 1 < raw_len ? (uint32_t)(raw[o] | (raw[o + 1] << 8)) : 0;
            v->fat[c] = c & 1 ? x >> 4 : x & 0xFFF;
        }
        free(raw);
        return true;
    }
    size_t es = v->fat_type == 16 ? 2 : 4, per = 16384;
    uint8_t *buf = malloc(per * es);
    if (!buf) { v->oom = true; return false; }
    for (size_t c = 0; c < L; c += per) {
        size_t k = L - c < per ? L - c : per;
        src_read(v->src, off + (uint64_t)c * es, buf, k * es);
        for (size_t i = 0; i < k; i++)
            v->fat[c + i] = es == 2 ? le16(buf + 2 * i) : le32(buf + 4 * i) & 0x0FFFFFFF;
    }
    free(buf);
    return true;
}

// Cluster chain from first into v->chain, stopping at EOC, free/bad/out of
// range values and loops.
static void chain(vol_t *v, uint64_t first) {
    v->chain_n = 0;
    uint64_t c = first;
    while (c >= 2 && c <= v->max_cluster) {
        if (c < v->fat_len) {
            if (v->seen[c >> 3] & (1u << (c & 7))) break;
            v->seen[c >> 3] |= (uint8_t)(1u << (c & 7));
        }
        if (++v->steps > MAX_CHAIN_STEPS) { limit(v, "cluster chain steps", true); break; }
        if (!grow((void **)&v->chain, &v->chain_cap, v->chain_n + 1, sizeof(uint32_t), &v->oom)) break;
        v->chain[v->chain_n++] = (uint32_t)c;
        c = fat_get(v, c);
    }
    for (size_t i = 0; i < v->chain_n; i++) {
        uint32_t x = v->chain[i];
        if (x < v->fat_len) v->seen[x >> 3] &= (uint8_t)~(1u << (x & 7));
    }
}

static void claim(vol_t *v, uint32_t idx) {
    for (size_t i = 0; i < v->chain_n; i++) {
        uint32_t c = v->chain[i];
        int32_t *own;
        uint32_t *pos;
        if (c < v->fat_len) {
            own = &v->owner[c];
            pos = &v->pos[c];
        } else {
            hent *h = hmap_get(&v->extra, c, true, &v->oom);
            if (!h) return;
            own = &h->owner;
            pos = &h->pos;
        }
        if (*own == -1) { *own = (int32_t)idx; *pos = (uint32_t)i; }
        else v->cross_links++;
    }
}

static int32_t owner_of(vol_t *v, uint64_t c, uint32_t *pos) {
    if (c < v->fat_len) { *pos = v->pos[c]; return v->owner[c]; }
    hent *h = c <= 0xFFFFFFFFu ? hmap_get(&v->extra, (uint32_t)c, false, &v->oom) : NULL;
    if (!h) return -1;
    *pos = h->pos;
    return h->owner;
}

static bool visited_test_set(vol_t *v, uint32_t fc) {
    if (hmap_get(&v->visited, fc, false, &v->oom)) return true;
    hmap_get(&v->visited, fc, true, &v->oom);
    return false;
}

static uint32_t new_obj(vol_t *v) {
    if (v->nobj >= MAX_OBJECTS) { limit(v, "objects", true); return NONE; }
    if (!grow((void **)&v->objs, &v->objcap, v->nobj + 1, sizeof(obj_t), &v->oom)) return NONE;
    obj_t *o = &v->objs[v->nobj];
    memset(o, 0, sizeof(*o));
    o->name = o->short_name = NONE;
    return (uint32_t)v->nobj++;
}

static bool add_ent(vol_t *v, uint32_t dir, uint32_t offset, uint32_t obj, uint32_t label) {
    if (!grow((void **)&v->ents, &v->entcap, v->nent + 1, sizeof(ent_t), &v->oom)) return false;
    v->ents[v->nent++] = (ent_t){offset, obj, label};
    v->objs[dir].ent_n++;
    return true;
}

static void push_job(vol_t *v, uint32_t obj, uint32_t fc, uint8_t mode, bool gone) {
    if (v->objs[obj].depth >= MAX_DEPTH) { limit(v, "directory depth", false); return; }
    if (!grow((void **)&v->jobs, &v->jobcap, v->njob + 1, sizeof(job_t), &v->oom)) return;
    v->jobs[v->njob++] = (job_t){obj, fc, mode, gone};
}

// One directory entry (8.3 slot plus the long-name slots before it).
typedef struct {
    unsigned n;                       // long-name slots pending
    uint8_t  first_chk;               // checksum byte of the first of them
    bool     same_chk, all_e5;        // all carry first_chk / all deleted
    uint8_t  last[32];                // the latest (= the name's first part)
    uint8_t  slots[MAX_LFN][32];      // in arrival order, while n <= MAX_LFN
} lfn_run;

static void handle_sfn(vol_t *v, uint32_t dir, bool gone, uint32_t offset, const uint8_t *e, lfn_run *lr) {
    bool deleted = e[0] == 0xE5;
    uint8_t raw[11];
    memcpy(raw, e, 11);
    if (raw[0] == 0x05) raw[0] = 0xE5;
    ustr longname;
    u_init(&longname);
    bool have_long = false;
    if (lr->n) {
        uint8_t want = sfn_checksum(e);
        if (deleted) {       // first byte lost: guess it from the long name
            uint32_t cps[13];
            size_t k = lfn_chars(lr->last, cps);
            uint8_t g[11];
            memcpy(g, e, 11);
            if (k) g[0] = upper_cp437_of(cps[0]);
            if (k && sfn_checksum(g) == lr->first_chk) {
                raw[0] = g[0];
                want = lr->first_chk;
            } else if (lr->all_e5 && lr->same_chk) {
                want = lr->first_chk;
            }
        }
        if (lr->first_chk == want && lr->same_chk && lr->n <= MAX_LFN) {
            for (unsigned i = lr->n; i-- > 0;) {
                uint32_t cps[13];
                size_t k = lfn_chars(lr->slots[i], cps);
                for (size_t j = 0; j < k; j++) u_put(&longname, cps[j]);
            }
            have_long = longname.len > 0;
        }
    }
    lr->n = 0;
    size_t bl = 8, el = 3;
    while (bl && raw[bl - 1] == ' ') bl--;
    while (el && raw[8 + el - 1] == ' ') el--;
    bool underscore = deleted && raw[0] == 0xE5;
    uint8_t nt = e[12];
    ustr shortn, shown;
    u_init(&shortn);
    u_init(&shown);
    for (size_t i = 0; i < bl; i++) {
        uint8_t ch = raw[i];
        uint32_t c = (i == 0 && underscore) ? '_' : cp437_ucs[ch];
        u_put(&shortn, c);
        u_put(&shown, (nt & 0x08) && !(i == 0 && underscore) ? cp437_lower_ucs[ch] : c);
    }
    if (el) {
        u_put(&shortn, '.');
        u_put(&shown, '.');
        for (size_t i = 0; i < el; i++) {
            uint8_t ch = raw[8 + i];
            u_put(&shortn, cp437_ucs[ch]);
            u_put(&shown, (nt & 0x10) ? cp437_lower_ucs[ch] : cp437_ucs[ch]);
        }
    }
    const char *name = have_long ? longname.s : shown.s;
    uint8_t attr = e[11];
    bool is_dir = attr & 0x10, is_label = (attr & 0x08) && !is_dir;
    bool dot = raw[0] == 0x2E && (!strcmp(shortn.s, ".") || !strcmp(shortn.s, ".."));
    bool del = deleted || gone;
    if (is_label) {
        uint32_t lab = pool_add(v, name);
        if (lab != NONE) add_ent(v, dir, offset, NONE, lab);
        return;
    }
    if (dot) return;
    uint32_t idx = new_obj(v);
    if (idx == NONE) return;
    obj_t o;
    memset(&o, 0, sizeof(o));
    o.parent = dir;
    o.depth = (uint16_t)(v->objs[dir].depth + 1);
    o.name = pool_add(v, name);
    o.short_name = strcmp(shortn.s, name) ? pool_add(v, shortn.s) : NONE;
    o.kind = is_dir ? K_DIR : K_FILE;
    o.deleted = del;
    o.has_size = !is_dir;
    o.size = le32(e + 28);
    o.first_cluster = ((v->fat_type == 32 ? (uint32_t)le16(e + 20) : 0) << 16) | le16(e + 26);
    o.mtime = le16(e + 22);
    o.mdate = le16(e + 24);
    char tb[24];
    o.has_mtime = fat_time(o.mdate, o.mtime, tb, sizeof(tb));
    v->objs[idx] = o;
    if (o.name == NONE) { v->nobj--; return; }
    add_ent(v, dir, offset, idx, NONE);
    uint32_t fc = o.first_cluster;
    if (fc < 2 || fc > v->max_cluster) return;
    if (!del) {
        chain(v, fc);
        claim(v, idx);
        if (is_dir && !visited_test_set(v, fc)) push_job(v, idx, fc, DIR_CHAIN, false);
    } else {
        // FAT keeps no chain for deleted files: assume contiguous clusters
        // from the first one (best effort), one cluster for a directory.
        uint64_t cnt = is_dir ? 1 : (o.size + (uint64_t)v->cluster_bytes - 1) / v->cluster_bytes;
        if (cnt < 1) cnt = 1;
        if (fc + cnt - 1 > v->max_cluster) return;          // implausible: runs past the volume
        if (!grow((void **)&v->dels, &v->delcap, v->ndel + 1, sizeof(del_t), &v->oom)) return;
        v->dels[v->ndel++] = (del_t){fc, (uint32_t)cnt, idx};
        if (is_dir && !hmap_get(&v->visited, fc, false, &v->oom)) {
            uint8_t blk[12];     // (v->clbuf holds the directory being parsed)
            src_read(v->src, v->start_lba * SECTOR + (v->data_start + (uint64_t)(fc - 2) * v->spc) * v->bps,
                     blk, sizeof(blk));
            if (!memcmp(blk, ".          ", 11) && (blk[11] & 0x10)) {
                visited_test_set(v, fc);
                push_job(v, idx, fc, DIR_ONE, true);
            }
        }
    }
}

// Parse 32-byte slots; returns false at the end-of-directory marker.
static bool parse_slots(vol_t *v, uint32_t dir, bool gone, const uint8_t *data, size_t len, uint64_t *off,
                        lfn_run *lr) {
    for (size_t p = 0; p + 32 <= len; p += 32, *off += 32) {
        if (*off >= (uint64_t)MAX_DIR_SLOTS * 32) { limit(v, "directory entries", false); return false; }
        if (v->stop || v->oom) return false;
        const uint8_t *e = data + p;
        if (e[0] == 0) return false;
        if ((e[11] & 0x3F) == 0x0F) {
            if (lr->n == 0) { lr->first_chk = e[13]; lr->same_chk = true; lr->all_e5 = true; }
            if (e[13] != lr->first_chk) lr->same_chk = false;
            if (e[0] != 0xE5) lr->all_e5 = false;
            if (lr->n < MAX_LFN) memcpy(lr->slots[lr->n], e, 32);
            memcpy(lr->last, e, 32);
            if (lr->n < UINT32_MAX) lr->n++;
            continue;
        }
        if (!plausible_sfn(e)) { lr->n = 0; continue; }
        handle_sfn(v, dir, gone, (uint32_t)*off, e, lr);
    }
    return true;
}

static void walk_dir(vol_t *v, const job_t *j) {
    v->objs[j->obj].ent_start = (uint32_t)v->nent;
    v->objs[j->obj].ent_n = 0;
    lfn_run *lr = calloc(1, sizeof(*lr));
    if (!lr) { v->oom = true; return; }
    uint64_t off = 0;
    if (j->mode == DIR_ROOT) {
        uint8_t *buf = v->clbuf;           // >= one sector
        for (uint64_t s = 0; s < v->b.root_secs; s++) {
            v->dir_bytes += v->bps;
            if (v->dir_bytes > MAX_DIR_BYTES) { limit(v, "directory data", true); break; }
            read_sectors(v, v->root_start + s, 1, buf);
            if (!parse_slots(v, j->obj, j->gone, buf, v->bps, &off, lr)) break;
        }
        free(lr);
        return;
    }
    // The clusters this directory can use at most (MAX_DIR_SLOTS entries).
    size_t want = (size_t)MAX_DIR_SLOTS * 32 / v->cluster_bytes + 1;
    uint32_t *cl = NULL;
    size_t n = 0;
    if (j->mode == DIR_ONE) {
        cl = malloc(sizeof(uint32_t));
        if (cl) { cl[0] = j->fc; n = 1; }
    } else {
        chain(v, j->fc);
        n = v->chain_n < want ? v->chain_n : want;
        cl = malloc((n ? n : 1) * sizeof(uint32_t));
        if (cl && n) memcpy(cl, v->chain, n * sizeof(uint32_t));
    }
    if (!cl) { free(lr); v->oom = true; return; }
    for (size_t i = 0; i < n; i++) {
        v->dir_bytes += v->cluster_bytes;
        if (v->dir_bytes > MAX_DIR_BYTES) { limit(v, "directory data", true); break; }
        read_sectors(v, v->data_start + (uint64_t)(cl[i] - 2) * v->spc, v->spc, v->clbuf);
        if (!parse_slots(v, j->obj, j->gone, v->clbuf, v->cluster_bytes, &off, lr)) break;
    }
    free(cl);
    free(lr);
}

// Walk all directories; map every allocated cluster to its owner.
static void build_index(vol_t *v) {
    if (v->indexed) return;
    v->indexed = true;
    if (!load_fat(v)) {
        if (v->oom && !v->error[0]) snprintf(v->error, sizeof(v->error), "out of memory indexing the volume");
        return;
    }
    uint32_t root = new_obj(v);
    if (root == NONE) return;
    v->objs[root].name = pool_add(v, "/");
    v->objs[root].kind = K_DIR;
    if (v->fat_type == 32) {
        chain(v, v->b.root_cluster);
        claim(v, root);
        v->objs[root].first_cluster = v->b.root_cluster;
        hmap_get(&v->visited, v->b.root_cluster, true, &v->oom);
        push_job(v, root, v->b.root_cluster, DIR_CHAIN, false);
    } else {
        push_job(v, root, 0, DIR_ROOT, false);
    }
    while (v->njob && !v->stop && !v->oom) {
        job_t j = v->jobs[--v->njob];
        walk_dir(v, &j);
    }
    if (v->oom) limit(v, "memory", true);
}

// "/" + names of the ancestors, as Python built it: parent.rstrip("/") + "/" + name.
static char *path_of(const vol_t *v, uint32_t oi) {
    uint32_t anc[MAX_DEPTH + 2];
    int n = 0;
    while (oi != 0 && n < MAX_DEPTH + 2) { anc[n++] = oi; oi = v->objs[oi].parent; }
    size_t cap = 2;
    for (int i = 0; i < n; i++) cap += strlen(pool_str(v, v->objs[anc[i]].name)) + 1;
    char *s = malloc(cap);
    if (!s) return NULL;
    strcpy(s, "/");
    size_t len = 1;
    for (int i = n - 1; i >= 0; i--) {
        while (len && s[len - 1] == '/') len--;
        s[len++] = '/';
        const char *nm = pool_str(v, v->objs[anc[i]].name);
        size_t k = strlen(nm);
        memcpy(s + len, nm, k);
        len += k;
        s[len] = '\0';
    }
    return s;
}

static cJSON *entries_in(const vol_t *v, uint32_t oi, uint64_t lo, uint64_t hi) {
    cJSON *a = cJSON_CreateArray();
    const obj_t *o = &v->objs[oi];
    for (uint32_t i = 0; i < o->ent_n; i++) {
        const ent_t *e = &v->ents[o->ent_start + i];
        if (e->offset < lo || e->offset >= hi) continue;
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "offset", e->offset);
        if (e->obj == NONE) {
            cJSON_AddStringToObject(x, "name", pool_str(v, e->label));
            cJSON_AddBoolToObject(x, "label", true);
        } else {
            const obj_t *c = &v->objs[e->obj];
            cJSON_AddStringToObject(x, "name", pool_str(v, c->name));
            cJSON_AddBoolToObject(x, "deleted", c->deleted);
            cJSON_AddBoolToObject(x, "dir", c->kind == K_DIR);
        }
        cJSON_AddItemToArray(a, x);
    }
    return a;
}

static void obj_record(const vol_t *v, uint32_t oi, uint64_t cluster, uint64_t byte_off, const char *kind,
                       cJSON *r) {
    const obj_t *o = &v->objs[oi];
    cJSON_AddStringToObject(r, "kind", kind ? kind : o->kind == K_DIR ? "directory" : "file");
    char *p = path_of(v, oi);
    cJSON_AddStringToObject(r, "path", p ? p : "?");
    free(p);
    cJSON_AddNumberToObject(r, "cluster", (double)cluster);
    if (o->short_name != NONE) cJSON_AddStringToObject(r, "short_name", pool_str(v, o->short_name));
    char tb[24];
    if (oi != 0 && o->has_mtime && fat_time(o->mdate, o->mtime, tb, sizeof(tb))) cJSON_AddStringToObject(r, "modified", tb);
    if (o->has_size) cJSON_AddNumberToObject(r, "size", o->size);
    if (o->kind == K_FILE) {
        cJSON_AddNumberToObject(r, "offset", (double)byte_off);
        cJSON_AddBoolToObject(r, "past_eof", byte_off >= o->size);
    } else {
        cJSON_AddBoolToObject(r, "is_dir", true);
        cJSON_AddItemToObject(r, "entries", entries_in(v, oi, byte_off, byte_off + SECTOR));
    }
}

static const del_t *g_sort_dels;          // qsort context (single-threaded use)
static int del_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t fx = g_sort_dels[x].fc, fy = g_sort_dels[y].fc;
    return fx != fy ? (fx < fy ? -1 : 1) : (x < y ? -1 : x > y);
}

// Index the deleted entries by first cluster, so a lookup only visits the
// entries that can cover a cluster.
static bool index_dels(vol_t *v) {
    if (v->del_order || !v->ndel) return v->ndel == 0 || v->del_order;
    v->del_order = malloc(v->ndel * sizeof(uint32_t));
    v->del_maxend = malloc(v->ndel * sizeof(uint64_t));
    if (!v->del_order || !v->del_maxend) {
        free(v->del_order); free(v->del_maxend);
        v->del_order = NULL; v->del_maxend = NULL;
        return false;
    }
    for (size_t i = 0; i < v->ndel; i++) v->del_order[i] = (uint32_t)i;
    g_sort_dels = v->dels;
    qsort(v->del_order, v->ndel, sizeof(uint32_t), del_cmp);
    uint64_t m = 0;
    for (size_t i = 0; i < v->ndel; i++) {
        const del_t *d = &v->dels[v->del_order[i]];
        if ((uint64_t)d->fc + d->n > m) m = (uint64_t)d->fc + d->n;
        v->del_maxend[i] = m;
    }
    return true;
}

typedef struct { uint64_t k; size_t i; uint32_t obj; } cand_t;
static int cand_cmp(const void *a, const void *b) {
    const cand_t *x = a, *y = b;
    if (x->k != y->k) return x->k < y->k ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

// What owns 512-byte image sector lba (inside the volume).
static void locate(vol_t *v, uint64_t lba, cJSON *r) {
    build_index(v);
    if (v->error[0]) {
        cJSON_AddStringToObject(r, "kind", "fs_error");
        cJSON_AddStringToObject(r, "detail", v->error);
        return;
    }
    uint64_t off = (lba - v->start_lba) * SECTOR, s = off / v->bps, within = off % v->bps;
    char det[160];
    if (s >= v->total_sectors) {
        cJSON_AddStringToObject(r, "kind", "partition_slack");
        cJSON_AddStringToObject(r, "detail", "past the end of the file system");
        return;
    }
    if (s == 0) { cJSON_AddStringToObject(r, "kind", "boot_sector"); return; }
    if (s < v->b.rsv) {
        if (v->fat_type == 32 && s == v->b.fsinfo) { cJSON_AddStringToObject(r, "kind", "fsinfo"); return; }
        uint64_t bk = v->b.backup;
        if (v->fat_type == 32 && bk && bk <= s && s < bk + 3) {
            cJSON_AddStringToObject(r, "kind", "backup_boot_sector");
            snprintf(det, sizeof(det), "backup boot record sector %llu", (unsigned long long)(s - bk));
            cJSON_AddStringToObject(r, "detail", det);
            return;
        }
        cJSON_AddStringToObject(r, "kind", "reserved");
        snprintf(det, sizeof(det), "reserved sector %llu", (unsigned long long)s);
        cJSON_AddStringToObject(r, "detail", det);
        return;
    }
    if (s < v->root_start) {
        uint64_t copy = (s - v->b.rsv) / v->b.fatsz, rel = (s - v->b.rsv) % v->b.fatsz;
        uint64_t bo = rel * v->bps + within;
        // int(bo / per) with per = 1.5, 2 or 4 bytes per entry
        uint64_t c0, c1;
        if (v->fat_type == 12) { c0 = bo * 2 / 3; c1 = (bo + SECTOR - 1) * 2 / 3; }
        else { unsigned per = v->fat_type == 16 ? 2 : 4; c0 = bo / per; c1 = (bo + SECTOR - 1) / per; }
        cJSON_AddStringToObject(r, "kind", "fat");
        cJSON_AddNumberToObject(r, "fat_copy", (double)(copy + 1));
        cJSON_AddNumberToObject(r, "fats", v->b.nfats);
        cJSON *cl = cJSON_AddArrayToObject(r, "clusters");
        cJSON_AddItemToArray(cl, cJSON_CreateNumber((double)c0));
        cJSON_AddItemToArray(cl, cJSON_CreateNumber((double)(c1 < v->max_cluster ? c1 : v->max_cluster)));
        if (c0 > v->max_cluster) cJSON_AddStringToObject(r, "detail", "unused tail of the FAT");
        else if (v->b.nfats > 1 && v->fat_mirrored)
            cJSON_AddStringToObject(r, "detail", "other FAT copies hold the same entries");
        return;
    }
    if (s < v->data_start) {
        uint64_t bo = (s - v->root_start) * v->bps + within;
        cJSON_AddStringToObject(r, "kind", "root_dir");
        cJSON_AddStringToObject(r, "path", "/");
        cJSON_AddBoolToObject(r, "is_dir", true);
        cJSON_AddItemToObject(r, "entries", v->nobj ? entries_in(v, 0, bo, bo + SECTOR) : cJSON_CreateArray());
        return;
    }
    uint64_t cl = (s - v->data_start) / v->spc + 2, rel = (s - v->data_start) % v->spc;
    if (cl > v->max_cluster) {
        cJSON_AddStringToObject(r, "kind", "volume_slack");
        cJSON_AddStringToObject(r, "detail", "data area past the last whole cluster");
        return;
    }
    uint64_t in_cl = rel * v->bps + within;
    uint32_t pos = 0;
    int32_t oi = owner_of(v, cl, &pos);
    if (oi >= 0) {
        obj_record(v, (uint32_t)oi, cl, (uint64_t)pos * v->cluster_bytes + in_cl, NULL, r);
        return;
    }
    uint32_t fv = fat_get(v, cl);
    const char *state = fv == 0 ? "free" : fv == v->bad_mark ? "bad" : "lost";
    // Deleted entries only claim clusters no live object owns.  The first
    // cluster is certain; the rest assume the file was contiguous.
    size_t nc = 0;
    cand_t *cands = NULL;
    if (index_dels(v) && v->ndel) {
        size_t lo = 0, hi = v->ndel;                  // first entry with fc > cl
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (v->dels[v->del_order[mid]].fc <= cl) lo = mid + 1;
            else hi = mid;
        }
        for (size_t j = lo; j-- > 0 && v->del_maxend[j] > cl;) {
            const del_t *d = &v->dels[v->del_order[j]];
            if (cl < (uint64_t)d->fc + d->n) {
                cand_t *nc2 = realloc(cands, (nc + 1) * sizeof(cand_t));
                if (!nc2) break;
                cands = nc2;
                cands[nc++] = (cand_t){cl - d->fc, v->del_order[j], d->obj};
            }
        }
    }
    if (nc) {
        qsort(cands, nc, sizeof(cand_t), cand_cmp);
        uint64_t k = cands[0].k;
        obj_record(v, cands[0].obj, cl, k * v->cluster_bytes + in_cl, "deleted", r);
        cJSON_AddBoolToObject(r, "best_effort", true);
        cJSON_AddStringToObject(r, "fat_entry", state);
        const char *tail = !strcmp(state, "free") ? "" : !strcmp(state, "bad")
                           ? "; cluster since marked bad in the FAT"
                           : "; cluster now allocated but unreachable (lost chain)";
        if (k == 0) snprintf(det, sizeof(det), "first cluster of the deleted entry%s", tail);
        else snprintf(det, sizeof(det), "cluster %llu of the deleted entry, assuming it was contiguous%s",
                      (unsigned long long)k, tail);
        cJSON_AddStringToObject(r, "detail", det);
        if (nc > 1) {
            cJSON *also = cJSON_AddArrayToObject(r, "also");
            for (size_t i = 1; i < nc; i++) {
                char *p = path_of(v, cands[i].obj);
                cJSON_AddItemToArray(also, cJSON_CreateString(p ? p : "?"));
                free(p);
            }
        }
        free(cands);
        return;
    }
    free(cands);
    if (fv == 0) {
        cJSON_AddStringToObject(r, "kind", "free");
        cJSON_AddNumberToObject(r, "cluster", (double)cl);
    } else if (fv == v->bad_mark) {
        cJSON_AddStringToObject(r, "kind", "bad_cluster");
        cJSON_AddNumberToObject(r, "cluster", (double)cl);
        cJSON_AddStringToObject(r, "detail", "marked bad in the FAT");
    } else {
        cJSON_AddStringToObject(r, "kind", "lost");
        cJSON_AddNumberToObject(r, "cluster", (double)cl);
        cJSON_AddStringToObject(r, "detail", "allocated in the FAT but not reachable from any directory");
    }
}

static cJSON *vol_info(const vol_t *v) {
    const bpb_t *b = &v->b;
    cJSON *o = cJSON_CreateObject();
    char fs[8];
    snprintf(fs, sizeof(fs), "FAT%d", v->fat_type);
    cJSON_AddStringToObject(o, "fs", fs);
    cJSON_AddStringToObject(o, "oem", b->oem);
    if (b->has_label) cJSON_AddStringToObject(o, "label", b->label);
    else cJSON_AddNullToObject(o, "label");
    cJSON_AddNumberToObject(o, "bytes_per_sector", v->bps);
    cJSON_AddNumberToObject(o, "sectors_per_cluster", v->spc);
    cJSON_AddNumberToObject(o, "cluster_bytes", v->cluster_bytes);
    cJSON_AddNumberToObject(o, "reserved_sectors", b->rsv);
    cJSON_AddNumberToObject(o, "fats", b->nfats);
    cJSON_AddNumberToObject(o, "fat_sectors", b->fatsz);
    cJSON_AddNumberToObject(o, "root_entries", b->rootent);
    cJSON_AddNumberToObject(o, "total_sectors", (double)v->total_sectors);
    cJSON_AddNumberToObject(o, "clusters", (double)b->clusters);
    cJSON_AddNumberToObject(o, "data_start_lba", (double)(v->start_lba + v->data_start * v->bps / SECTOR));
    if (v->fat_type == 32) cJSON_AddNumberToObject(o, "root_cluster", b->root_cluster);
    if (v->indexed && v->error[0]) {
        cJSON_AddStringToObject(o, "error", v->error);
    } else if (v->indexed) {
        size_t files = 0, dirs = 0, live = 0;
        for (size_t i = 1; i < v->nobj; i++) {
            if (v->objs[i].deleted) continue;
            live++;
            if (v->objs[i].kind == K_FILE) files++; else dirs++;
        }
        cJSON_AddNumberToObject(o, "files", (double)files);
        cJSON_AddNumberToObject(o, "directories", (double)dirs);
        cJSON_AddNumberToObject(o, "deleted_entries", (double)(v->nobj ? v->nobj - 1 - live : 0));
        cJSON_AddNumberToObject(o, "cross_links", (double)v->cross_links);
        if (v->trunc) {
            cJSON_AddBoolToObject(o, "index_truncated", true);
            cJSON_AddStringToObject(o, "index_limit", v->trunc);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
//  Disk layout
// ---------------------------------------------------------------------------

static const struct { uint8_t t; const char *name; } part_types[] = {
    {0x00, "empty"}, {0x01, "FAT12"}, {0x04, "FAT16 <32M"}, {0x05, "extended"}, {0x06, "FAT16"},
    {0x07, "NTFS/HPFS/exFAT"}, {0x0B, "FAT32"}, {0x0C, "FAT32 LBA"}, {0x0E, "FAT16 LBA"}, {0x0F, "extended LBA"},
    {0x11, "hidden FAT12"}, {0x12, "Compaq diagnostics"}, {0x14, "hidden FAT16 <32M"}, {0x16, "hidden FAT16"},
    {0x17, "hidden NTFS/HPFS"}, {0x1B, "hidden FAT32"}, {0x1C, "hidden FAT32 LBA"}, {0x1E, "hidden FAT16 LBA"},
    {0x27, "recovery"}, {0x42, "Windows dynamic"}, {0x63, "Unix System V"}, {0x82, "Linux swap / Solaris"},
    {0x83, "Linux"}, {0x84, "hibernation"}, {0x85, "Linux extended"}, {0x8E, "Linux LVM"}, {0xA0, "hibernation"},
    {0xA5, "FreeBSD"}, {0xA6, "OpenBSD"}, {0xA9, "NetBSD"}, {0xAF, "HFS/HFS+"}, {0xBE, "Solaris boot"},
    {0xBF, "Solaris"}, {0xDE, "Dell utility"}, {0xEE, "GPT protective"}, {0xEF, "EFI system"}, {0xFD, "Linux RAID"},
};
static const char *part_type_name(int t) {
    for (size_t i = 0; i < sizeof(part_types) / sizeof(part_types[0]); i++)
        if (part_types[i].t == t) return part_types[i].name;
    return "unknown";
}
static bool fat_part_type(int t) {
    static const uint8_t f[] = {0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0x11, 0x14, 0x16, 0x1B, 0x1C, 0x1E};
    for (size_t i = 0; i < sizeof(f); i++) if (f[i] == t) return true;
    return false;
}
static bool ext_part_type(int t) { return t == 0x05 || t == 0x0F || t == 0x85; }

typedef struct {
    char     name[16];
    int      index, type;           // type -1 = none (superfloppy)
    uint64_t start, sectors;
    bool     boot, extended, logical;
    int      fat;                   // 0, 12, 16, 32
    char     note[96];
    vol_t   *vol;
} part_t;

typedef struct {
    uint64_t    image_sectors;
    const char *scheme;
    part_t     *parts;
    size_t      nparts, cap;
    uint64_t   *ebrs;
    size_t      nebr, ebrcap;
    bool        oom;
} layout_t;

static part_t *add_part(layout_t *l) {
    if (!grow((void **)&l->parts, &l->cap, l->nparts + 1, sizeof(part_t), &l->oom)) return NULL;
    part_t *p = &l->parts[l->nparts++];
    memset(p, 0, sizeof(*p));
    return p;
}

static int64_t part_end(const part_t *p) { return (int64_t)(p->start + p->sectors) - 1; }

static void disk_layout(src_t *src, layout_t *l) {
    memset(l, 0, sizeof(*l));
    l->image_sectors = src->size / SECTOR;
    uint8_t mbr[SECTOR];
    src_read(src, 0, mbr, SECTOR);
    bpb_t b;
    bool bpb_ok = parse_bpb(mbr, &b);
    bool sig = mbr[510] == 0x55 && mbr[511] == 0xAA, all_boot = true, any = false;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) all_boot = false;
        if (e[4] && le32(e + 12)) any = true;
    }
    bool table_ok = sig && all_boot && any;
    if (bpb_ok && (!table_ok || !memcmp(mbr + 54, "FAT12", 5) || !memcmp(mbr + 54, "FAT16", 5) ||
                   !memcmp(mbr + 82, "FAT32", 5))) {
        l->scheme = "superfloppy";
        part_t *p = add_part(l);
        if (p) {
            strcpy(p->name, "vol");
            p->type = -1;
            p->sectors = l->image_sectors;
        }
    } else if (sig) {
        l->scheme = "mbr";
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 446 + 16 * i;
            int typ = e[4];
            uint32_t start = le32(e + 8), cnt = le32(e + 12);
            if (typ == 0 || cnt == 0) continue;
            part_t *p = add_part(l);
            if (!p) return;
            snprintf(p->name, sizeof(p->name), "p%d", i + 1);
            p->index = i + 1;
            p->type = typ;
            p->start = start;
            p->sectors = cnt;
            p->boot = e[0] == 0x80;
            if (!ext_part_type(typ)) continue;
            p->extended = true;
            int n = 5;
            uint64_t ebr = start;
            size_t seen_from = l->nebr;
            while (ebr && ebr < l->image_sectors && l->nebr - seen_from < 256) {
                bool again = false;
                for (size_t k = seen_from; k < l->nebr; k++) if (l->ebrs[k] == ebr) again = true;
                if (again) break;
                if (!grow((void **)&l->ebrs, &l->ebrcap, l->nebr + 1, sizeof(uint64_t), &l->oom)) return;
                l->ebrs[l->nebr++] = ebr;
                uint8_t eb[SECTOR];
                src_read(src, ebr * SECTOR, eb, SECTOR);
                if (eb[510] != 0x55 || eb[511] != 0xAA) break;
                int t1 = eb[446 + 4], t2 = eb[462 + 4];
                uint32_t s1 = le32(eb + 446 + 8), c1 = le32(eb + 446 + 12), s2 = le32(eb + 462 + 8);
                if (t1 && c1) {
                    part_t *q = add_part(l);
                    if (!q) return;
                    snprintf(q->name, sizeof(q->name), "p%d", n);
                    q->index = n;
                    q->type = t1;
                    q->start = ebr + s1;
                    q->sectors = c1;
                    q->logical = true;
                    n++;
                }
                ebr = ext_part_type(t2) && s2 ? (uint64_t)start + s2 : 0;
            }
        }
    } else {
        l->scheme = "none";
    }
    for (size_t i = 0; i < l->nparts; i++) {
        part_t *p = &l->parts[i];
        if (p->extended) continue;
        bool ok = false;
        if (p->start < l->image_sectors) {
            uint8_t bs[SECTOR];
            src_read(src, p->start * SECTOR, bs, SECTOR);
            ok = parse_bpb(bs, &b);
        }
        if (ok) {
            p->fat = b.fat_type;
            if (p->type >= 0 && !fat_part_type(p->type))
                snprintf(p->note, sizeof(p->note), "type 0x%02X but holds a FAT%d file system", p->type, b.fat_type);
        } else if (p->type >= 0 && fat_part_type(p->type)) {
            snprintf(p->note, sizeof(p->note), "FAT partition type but no valid FAT boot sector");
        }
    }
}

static void free_layout(layout_t *l) {
    for (size_t i = 0; i < l->nparts; i++)
        if (l->parts[i].vol) { vol_free(l->parts[i].vol); free(l->parts[i].vol); }
    free(l->parts);
    free(l->ebrs);
}

static cJSON *part_json(const part_t *p) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddNumberToObject(o, "index", p->index);
    if (p->type >= 0) {
        cJSON_AddNumberToObject(o, "type", p->type);
        cJSON_AddStringToObject(o, "type_name", part_type_name(p->type));
    } else {
        cJSON_AddNullToObject(o, "type");
        cJSON_AddStringToObject(o, "type_name", "superfloppy");
    }
    cJSON_AddNumberToObject(o, "start_lba", (double)p->start);
    cJSON_AddNumberToObject(o, "sectors", (double)p->sectors);
    cJSON_AddBoolToObject(o, "boot", p->boot);
    if (p->extended) cJSON_AddBoolToObject(o, "extended", true);
    if (p->logical) cJSON_AddBoolToObject(o, "logical", true);
    cJSON_AddNumberToObject(o, "end_lba", (double)part_end(p));
    if (p->fat) {
        char fs[8];
        snprintf(fs, sizeof(fs), "FAT%d", p->fat);
        cJSON_AddStringToObject(o, "fat", fs);
    } else {
        cJSON_AddNullToObject(o, "fat");
    }
    if (p->note[0]) cJSON_AddStringToObject(o, "note", p->note);
    if (p->vol) cJSON_AddItemToObject(o, "volume", vol_info(p->vol));
    return o;
}

// ---------------------------------------------------------------------------
//  whatfile
// ---------------------------------------------------------------------------

typedef struct { int64_t v; size_t i; } lba_ix;
static int lba_ix_cmp(const void *a, const void *b) {
    const lba_ix *x = a, *y = b;
    if (x->v != y->v) return x->v < y->v ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

// String-keyed table for the per-object summary.
typedef struct { char *key; cJSON *obj; } skey;
typedef struct { skey *t; size_t cap, n; } smap;

static size_t str_hash(const char *s) {
    size_t h = 1469598103u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static skey *smap_get(smap *m, const char *key, bool *oom) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 64;
        skey *nt = calloc(nc, sizeof(skey));
        if (!nt) { *oom = true; return NULL; }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->t[i].key) continue;
            size_t j = str_hash(m->t[i].key) & (nc - 1);
            while (nt[j].key) j = (j + 1) & (nc - 1);
            nt[j] = m->t[i];
        }
        free(m->t);
        m->t = nt;
        m->cap = nc;
    }
    size_t j = str_hash(key) & (m->cap - 1);
    while (m->t[j].key) {
        if (!strcmp(m->t[j].key, key)) return &m->t[j];
        j = (j + 1) & (m->cap - 1);
    }
    m->t[j].key = malloc(strlen(key) + 1);
    if (!m->t[j].key) { *oom = true; return NULL; }
    strcpy(m->t[j].key, key);
    m->t[j].obj = NULL;
    m->n++;
    return &m->t[j];
}

static void smap_free(smap *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->t[i].key);
    free(m->t);
}

static int whatfile_src(src_t *src, const char *image_name, const int64_t *lbas, size_t n, cJSON **out) {
    *out = NULL;
    if (n > AB_WHATFILE_MAX_LBAS)
        return ab_fail(AB_ERR_ARG, "too many LBAs (%zu, limit %u)", n, (unsigned)AB_WHATFILE_MAX_LBAS);
    // Drop repeats, keep the first occurrence's position.
    lba_ix *ix = malloc((n ? n : 1) * sizeof(lba_ix));
    bool *keep = calloc(n ? n : 1, 1);
    if (!ix || !keep) { free(ix); free(keep); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    for (size_t i = 0; i < n; i++) ix[i] = (lba_ix){lbas[i], i};
    qsort(ix, n, sizeof(lba_ix), lba_ix_cmp);
    for (size_t i = 0; i < n; i++) if (i == 0 || ix[i].v != ix[i - 1].v) keep[ix[i].i] = true;
    free(ix);

    layout_t lay;
    disk_layout(src, &lay);
    uint64_t primary_start = lay.image_sectors;
    bool any_primary = false;
    for (size_t i = 0; i < lay.nparts; i++)
        if (!lay.parts[i].logical && (!any_primary || lay.parts[i].start < primary_start)) {
            primary_start = lay.parts[i].start;
            any_primary = true;
        }

    cJSON *results = cJSON_CreateArray();
    bool oom = lay.oom;
    for (size_t li = 0; li < n && !oom; li++) {
        if (!keep[li]) continue;
        int64_t lba = lbas[li];
        cJSON *r = cJSON_CreateObject();
        if (!r) { oom = true; break; }
        cJSON_AddNumberToObject(r, "lba", (double)lba);
        part_t *inner = NULL, *hit = NULL;
        for (size_t i = 0; i < lay.nparts; i++) {
            part_t *p = &lay.parts[i];
            if (lba >= 0 && (uint64_t)lba >= p->start && lba <= part_end(p)) {
                if (!hit) hit = p;
                if (!p->extended && !inner) inner = p;
            }
        }
        bool is_ebr = false;
        for (size_t i = 0; i < lay.nebr && lba >= 0; i++) if (lay.ebrs[i] == (uint64_t)lba) is_ebr = true;
        if (lba < 0) {
            cJSON_AddStringToObject(r, "kind", "invalid");
        } else if ((uint64_t)lba >= lay.image_sectors) {
            cJSON_AddStringToObject(r, "kind", "beyond_image");
        } else if (inner) {
            cJSON_AddStringToObject(r, "partition", inner->name);
            if (inner->fat) {
                if (!inner->vol) {
                    inner->vol = calloc(1, sizeof(vol_t));
                    if (!inner->vol || !vol_open(inner->vol, src, inner->start)) {
                        // parse_bpb accepted this sector a moment ago: only OOM gets here
                        free(inner->vol);
                        inner->vol = NULL;
                        oom = true;
                        cJSON_Delete(r);
                        break;
                    }
                    build_index(inner->vol);
                }
                char fs[8];
                snprintf(fs, sizeof(fs), "FAT%d", inner->fat);
                cJSON_AddStringToObject(r, "fs", fs);
                if ((uint64_t)lba < vol_end_lba(inner->vol)) {
                    locate(inner->vol, (uint64_t)lba, r);
                } else {
                    cJSON_AddStringToObject(r, "kind", "partition_slack");
                    cJSON_AddStringToObject(r, "detail", "inside the partition, past the file system");
                }
            } else {
                cJSON_AddStringToObject(r, "kind", "non_fat_partition");
                if (inner->type >= 0) cJSON_AddNumberToObject(r, "type", inner->type);
                else cJSON_AddNullToObject(r, "type");
                cJSON_AddStringToObject(r, "type_name", inner->type >= 0 ? part_type_name(inner->type) : "superfloppy");
            }
        } else if (lba == 0 && !strcmp(lay.scheme, "mbr")) {
            cJSON_AddStringToObject(r, "kind", "mbr");
        } else if (is_ebr) {
            cJSON_AddStringToObject(r, "kind", "ebr");
            cJSON_AddStringToObject(r, "detail", "extended boot record (logical partition table)");
        } else if (hit) {
            cJSON_AddStringToObject(r, "kind", "unpartitioned");
            cJSON_AddStringToObject(r, "partition", hit->name);
            cJSON_AddStringToObject(r, "detail", "inside the extended partition, outside any logical drive");
        } else if (!strcmp(lay.scheme, "mbr") && (uint64_t)lba < primary_start) {
            cJSON_AddStringToObject(r, "kind", "mbr_gap");
            cJSON_AddStringToObject(r, "detail", "between the partition table and the first partition");
        } else {
            cJSON_AddStringToObject(r, "kind", "unpartitioned");
        }
        cJSON_AddItemToArray(results, r);
    }
    free(keep);
    if (src->err) {
        cJSON_Delete(results);
        free_layout(&lay);
        return ab_fail(AB_ERR_IO, "read error in %s", image_name);
    }
    if (oom) {
        cJSON_Delete(results);
        free_layout(&lay);
        return ab_fail(AB_ERR_NOMEM, "out of memory");
    }

    cJSON *parts = cJSON_CreateArray();
    for (size_t i = 0; i < lay.nparts; i++) cJSON_AddItemToArray(parts, part_json(&lay.parts[i]));

    // Per-object summary and counts per kind, in order of first appearance.
    cJSON *files = cJSON_CreateArray(), *counts = cJSON_CreateObject();
    smap objs = {0};
    for (cJSON *r = results->child; r && !oom; r = r->next) {
        const char *kind = ab_jstr(r, "kind", "");
        cJSON *c = cJSON_GetObjectItemCaseSensitive(counts, kind);
        if (c) cJSON_SetNumberValue(c, c->valuedouble + 1);
        else cJSON_AddNumberToObject(counts, kind, 1);
        if (strcmp(kind, "file") && strcmp(kind, "directory") && strcmp(kind, "deleted") && strcmp(kind, "root_dir"))
            continue;
        const char *part = ab_jstr(r, "partition", NULL), *path = ab_jstr(r, "path", "");
        size_t kl = strlen(part ? part : "") + strlen(path) + strlen(kind) + 4;
        char *key = malloc(kl);
        if (!key) { oom = true; break; }
        snprintf(key, kl, "%c%s\x01%s\x01%s", part ? 'p' : 'n', part ? part : "", path, kind);
        skey *e = smap_get(&objs, key, &oom);
        free(key);
        if (!e) break;
        if (!e->obj) {
            cJSON *o = cJSON_CreateObject();
            if (part) cJSON_AddStringToObject(o, "partition", part);
            else cJSON_AddNullToObject(o, "partition");
            cJSON_AddStringToObject(o, "path", path);
            cJSON_AddStringToObject(o, "kind", kind);
            cJSON_AddArrayToObject(o, "lbas");
            const cJSON *sz = cJSON_GetObjectItemCaseSensitive(r, "size");
            if (cJSON_IsNumber(sz)) cJSON_AddNumberToObject(o, "size", sz->valuedouble);
            if (!strcmp(kind, "file")) cJSON_AddBoolToObject(o, "slack_only", true);
            cJSON_AddItemToArray(files, o);
            e->obj = o;
        }
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(e->obj, "lbas"),
                             cJSON_CreateNumber(ab_jnum(r, "lba", 0)));
        if (!strcmp(kind, "file") && !ab_jbool(r, "past_eof", false))
            cJSON_ReplaceItemInObjectCaseSensitive(e->obj, "slack_only", cJSON_CreateFalse());
    }
    smap_free(&objs);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "result", "ok");
    cJSON_AddStringToObject(o, "image", image_name);
    cJSON_AddNumberToObject(o, "image_sectors", (double)lay.image_sectors);
    cJSON_AddStringToObject(o, "scheme", lay.scheme);
    cJSON_AddItemToObject(o, "partitions", parts);
    cJSON_AddItemToObject(o, "results", results);
    cJSON_AddItemToObject(o, "files", files);
    cJSON_AddItemToObject(o, "counts", counts);
    free_layout(&lay);
    if (oom) { cJSON_Delete(o); return ab_fail(AB_ERR_NOMEM, "out of memory"); }
    *out = o;
    return AB_OK;
}

int ab_whatfile(const char *image_path, const int64_t *lbas, size_t n, cJSON **out) {
    *out = NULL;
    FILE *f = fopen(image_path, "rb");
    if (!f) return ab_fail(AB_ERR_IO, "[Errno %d] %s: '%s'", errno, strerror(errno), image_path);
    src_t s = {f, NULL, 0, false};
    if (ab_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ab_fail(AB_ERR_IO, "cannot seek in %s", image_path);
    }
    long long size = (long long)ab_ftell(f);
    s.size = size > 0 ? (uint64_t)size : 0;
    int rc = whatfile_src(&s, image_path, lbas, n, out);
    fclose(f);
    return rc;
}

int ab_whatfile_mem(const uint8_t *img, size_t len, const char *image_name, const int64_t *lbas, size_t n,
                    cJSON **out) {
    src_t s = {NULL, img, len, false};
    return whatfile_src(&s, image_name ? image_name : "", lbas, n, out);
}

// ---------------------------------------------------------------------------
//  LBA lists: command line, ddrescue mapfile, JSON file
// ---------------------------------------------------------------------------

typedef struct { int64_t *v; size_t n, cap; bool oom; } lbalist;

static int lba_push(lbalist *l, int64_t x) {
    if (l->n >= AB_WHATFILE_MAX_LBAS)
        return ab_fail(AB_ERR_ARG, "too many LBAs (limit %u)", (unsigned)AB_WHATFILE_MAX_LBAS);
    if (!grow((void **)&l->v, &l->cap, l->n + 1, sizeof(int64_t), &l->oom)) return ab_fail(AB_ERR_NOMEM, "out of memory");
    l->v[l->n++] = x;
    return AB_OK;
}

// Python int(s, 0) for an LBA (no underscores); false if not a number.
static bool parse_int0(const char *s, int64_t *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    if (!*s) return false;
    if (base == 10 && s[0] == '0') {               // Python: "0", "00" ok, "012" not
        for (const char *p = s; *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t'; p++)
            if (*p != '0') return false;
    }
    uint64_t v = 0;
    const char *p = s;
    for (; *p; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10
              : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10 : 99;
        if (d >= base) break;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return false;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p || v > (uint64_t)INT64_MAX) return false;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

// Every sector of the mapfile's tried-and-failed areas: status other than
// '+' (finished) and '?' (never tried), like Python's Mapfile.bad_sectors().
static int mapfile_bad(const char *path, lbalist *l) {
    FILE *f = fopen(path, "r");
    if (!f) return ab_fail(AB_ERR_IO, "[Errno %d] %s: '%s'", errno, strerror(errno), path);
    char line[512];
    bool have_cur = false;
    int rc = AB_OK, ln = 0;
    while (rc == AB_OK && fgets(line, sizeof(line), f)) {
        ln++;
        size_t L = strlen(line);
        if (L == sizeof(line) - 1 && line[L - 1] != '\n') {      // overlong line: skip the rest
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
        }
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s || *s == '#') continue;
        if (!have_cur) { have_cur = true; continue; }        // current-position line
        char a[64], b[64], st[16];
        if (sscanf(s, "%63s %63s %15s", a, b, st) != 3) { rc = ab_fail(AB_ERR_ARG, "%s:%d: bad mapfile line", path, ln); break; }
        int64_t pos, size;
        if (!parse_int0(a, &pos) || !parse_int0(b, &size) || pos < 0 || size < 0) {
            rc = ab_fail(AB_ERR_ARG, "%s:%d: bad mapfile line", path, ln);
            break;
        }
        if (!strcmp(st, "+") || !strcmp(st, "?")) continue;
        uint64_t first = (uint64_t)pos / SECTOR, end = ((uint64_t)pos + (uint64_t)size + SECTOR - 1) / SECTOR;
        if (end > first && end - first > AB_WHATFILE_MAX_LBAS) {
            rc = ab_fail(AB_ERR_ARG, "too many LBAs (limit %u)", (unsigned)AB_WHATFILE_MAX_LBAS);
            break;
        }
        for (uint64_t x = first; x < end && rc == AB_OK; x++) rc = lba_push(l, (int64_t)x);
    }
    fclose(f);
    if (rc == AB_OK && !have_cur) rc = ab_fail(AB_ERR_ARG, "%s: not a ddrescue mapfile", path);
    return rc;
}

static int json_lbas(const char *path, lbalist *l) {
    FILE *f = fopen(path, "rb");
    if (!f) return ab_fail(AB_ERR_IO, "[Errno %d] %s: '%s'", errno, strerror(errno), path);
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    int rc = AB_OK;
    while (buf) {
        if (len + 4096 > cap) {
            if (cap >= (256u << 20)) { rc = ab_fail(AB_ERR_ARG, "%s: too large", path); break; }
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    if (!buf) return ab_fail(AB_ERR_NOMEM, "out of memory");
    if (rc != AB_OK) { free(buf); return rc; }
    cJSON *j = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!j) return ab_fail(AB_ERR_ARG, "%s: not valid JSON", path);
    const cJSON *a = j;
    if (cJSON_IsObject(j)) {
        a = cJSON_GetObjectItemCaseSensitive(j, "bad_lbas");
        if (!a) a = cJSON_GetObjectItemCaseSensitive(j, "lbas");
        if (!a) a = NULL;
    }
    if (cJSON_IsObject(j) && !a) {                 // j.get("lbas", []) -> empty list
        cJSON_Delete(j);
        return AB_OK;
    }
    if (!cJSON_IsArray(a)) {
        cJSON_Delete(j);
        return ab_fail(AB_ERR_ARG, "%s: expected a list of LBAs or an object with \"bad_lbas\"", path);
    }
    for (const cJSON *x = a->child; x && rc == AB_OK; x = x->next) {
        int64_t v;
        const cJSON *y = x;
        if (cJSON_IsObject(x)) {
            y = cJSON_GetObjectItemCaseSensitive(x, "lba");
            if (!y) { rc = ab_fail(AB_ERR_ARG, "%s: entry without \"lba\"", path); break; }
        }
        if (cJSON_IsString(y) && !cJSON_IsObject(x)) {
            if (!parse_int0(y->valuestring, &v)) {
                rc = ab_fail(AB_ERR_ARG, "invalid literal for int() with base 0: '%s'", y->valuestring);
                break;
            }
        } else if (cJSON_IsNumber(y) && y->valuedouble > -9.2e18 && y->valuedouble < 9.2e18) {
            v = (int64_t)y->valuedouble;            // int(): truncates
        } else if (cJSON_IsBool(y)) {
            v = cJSON_IsTrue(y) ? 1 : 0;
        } else {
            rc = ab_fail(AB_ERR_ARG, "%s: not an LBA in the list", path);
            break;
        }
        rc = lba_push(l, v);
    }
    cJSON_Delete(j);
    return rc;
}

int ab_whatfile_lbas(const int64_t *lbas, size_t n, const char *mapfile, const char *json_file,
                     int64_t **out, size_t *nout) {
    *out = NULL;
    *nout = 0;
    lbalist l = {0};
    int rc = AB_OK;
    for (size_t i = 0; i < n && rc == AB_OK; i++) rc = lba_push(&l, lbas[i]);
    if (rc == AB_OK && mapfile && *mapfile) rc = mapfile_bad(mapfile, &l);
    if (rc == AB_OK && json_file && *json_file) rc = json_lbas(json_file, &l);
    if (rc != AB_OK) { free(l.v); return rc; }
    *out = l.v;
    *nout = l.n;
    return AB_OK;
}
