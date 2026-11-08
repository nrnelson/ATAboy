#ifndef JSON_H
#define JSON_H

// Minimal bounded JSON writer for API replies.  Never writes past 'cap';
// on overflow it sets 'overflow' and stops emitting.

#include <stdint.h>
#include <stdbool.h>

#define JSON_MAX_DEPTH 8

typedef struct {
    char    *buf;
    uint32_t cap, len;
    uint8_t  depth;
    bool     comma[JSON_MAX_DEPTH];   // next value at this depth needs a comma
    char     close[JSON_MAX_DEPTH];   // '}' or ']'
    bool     overflow;
} json_t;

typedef struct { uint32_t len; uint8_t depth; bool comma; } json_mark_t;

void j_init(json_t *j, char *buf, uint32_t cap);
void j_obj(json_t *j, const char *key);          // key = NULL inside arrays / top level
void j_arr(json_t *j, const char *key);
void j_end(json_t *j);
void j_int(json_t *j, const char *key, int64_t v);
void j_uint(json_t *j, const char *key, uint64_t v);
void j_bool(json_t *j, const char *key, bool v);
void j_str(json_t *j, const char *key, const char *s);
void j_null(json_t *j, const char *key);
json_mark_t j_mark(const json_t *j);
void j_restore(json_t *j, json_mark_t m);

// "BSY|DRDY|..." / "UNC|ABRT|..." decoders ("" when no bits set)
const char *ata_status_bits(uint8_t st, char *out);
const char *ata_error_bits(uint8_t err, char *out);
const char *ide_event_bits(uint8_t ev, char *out);   // IDE_EV_*, out >= 96 bytes

#endif
