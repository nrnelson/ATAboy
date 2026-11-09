#ifndef AB_INTERNAL_H
#define AB_INTERNAL_H

// Private to libataboy.

#include "ataboy.h"
#include <stdarg.h>

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

#define AB_MSG_LEN 512

// Format the thread's last error message and return err.
#if defined(__MINGW32__)
#define AB_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__)
#define AB_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define AB_PRINTF(f, a)
#endif
int  ab_fail(int err, const char *fmt, ...) AB_PRINTF(2, 3);
void ab_vfail(const char *fmt, va_list ap) AB_PRINTF(1, 0);

// ---------------------------------------------------------------------------
//  Transports
// ---------------------------------------------------------------------------

// One API request: send the 16-byte CDB, receive up to 'alloc' reply bytes
// into out (*out_len set).  Returns AB_OK, or:
//   AB_ERR_TRANSPORT with *maybe_executed: false if the device certainly
//     didn't run it (write failed, frame rejected as corrupt), true if the
//     reply was lost after sending;
//   AB_ERR_CHECK with sense in *sense (key, asc, ascq);
//   AB_ERR_PERM.
typedef struct ab_transport {
    const char *name;
    int  (*api)(struct ab_transport *t, const uint8_t cdb[16], uint32_t alloc, uint32_t timeout_ms,
                uint8_t *out, size_t *out_len, bool *maybe_executed, uint8_t sense[3]);
    void (*close)(struct ab_transport *t);
} ab_transport;

int ab_serial_transport_open(const char *path, ab_transport **out);
#if defined(__linux__)
int ab_sgio_transport_open(const char *path, ab_transport **out);
#endif

// ---------------------------------------------------------------------------
//  Platform layer (plat_posix.c / plat_win.c)
// ---------------------------------------------------------------------------

uint64_t ab_now_ms(void);                 // monotonic
void     ab_sleep_ms(uint32_t ms);
uint32_t ab_random32(void);               // non-zero not guaranteed

// Serial port: raw 8N1, no flow control, exclusive where the OS allows.
typedef struct ab_serial ab_serial;
int  ab_serial_open(const char *path, ab_serial **out);
// Write all of buf before deadline (ab_now_ms based).  AB_OK / AB_ERR_TRANSPORT.
int  ab_serial_write(ab_serial *s, const uint8_t *buf, size_t n, uint64_t deadline_ms);
// Read what is available, waiting until at least 1 byte or the deadline.
// Returns bytes read (0 = deadline passed) or a negative ab_err.
long ab_serial_read(ab_serial *s, uint8_t *buf, size_t cap, uint64_t deadline_ms);
void ab_serial_drain(ab_serial *s);       // discard pending input
void ab_serial_close(ab_serial *s);
bool ab_is_serial_path(const char *path);

// ---------------------------------------------------------------------------
//  OS integration (discover_*.c)
// ---------------------------------------------------------------------------

// Make the host OS notice a mount / write-protect change and wait (up to
// wait_ms) until its view matches: 'sectors' capacity (0 = no medium), and
// read-only == want_ro when want_ro >= 0.  No-op where not applicable.
void ab_os_rescan(const ab_devinfo *d, uint64_t sectors, int want_ro, uint32_t wait_ms);
// Make the OS's SCSI command timeout at least need_s (never lowers it).
// Returns the timeout in effect, or -1 if it can't be read/set.
int  ab_os_ensure_scsi_timeout(ab_devinfo *d, int need_s);

// ---------------------------------------------------------------------------
//  The device
// ---------------------------------------------------------------------------

struct ab_dev {
    ab_devinfo    info;
    bool          have_info;
    ab_transport *tp;
    char          path[256];
    int           ext;                    // -1 unknown, 0 basic only, 1 extended
    bool          have_boot_id;
    uint32_t      boot_id, state_counter;
    unsigned      reboots;
    bool          have_last_reboot;
    uint32_t      last_old_boot, last_new_boot, last_reboot_uptime;
    uint8_t       last_reboot_op;
    unsigned      empty_replies;
    ab_reboot_cb  on_reboot;
    void         *on_reboot_ctx;
    uint8_t       sense[3];
    char          msg[AB_MSG_LEN];
    int           max_read_sectors;       // from INFO, 0 = not asked yet
};

// Record the thread's last error message as dev's and return err.
int ab_dev_fail(ab_dev *dev, int err);

// 64-bit file positioning
#include <stdio.h>
#if defined(_WIN32)
#define ab_fseek _fseeki64
#define ab_ftell _ftelli64
#else
#define ab_fseek fseeko
#define ab_ftell ftello
#endif

double ab_round(double v, int digits);    // Python round() for display values

// Files (plat_posix.c / plat_win.c)
uint64_t ab_now_us(void);                 // monotonic, microseconds
bool ab_file_exists(const char *path);
// fflush + fsync (_commit on Windows).  AB_OK / AB_ERR_IO (message set).
int  ab_file_sync(FILE *f, const char *path);
// Grow the file to at least size bytes (sparse where the OS can); never shrinks.
int  ab_file_extend(FILE *f, const char *path, uint64_t size);
// Atomically replace dst with tmp (rename; MoveFileEx on Windows).
int  ab_file_replace(const char *tmp, const char *dst);
// When running as root under sudo, hand a created file back to the invoking
// user (SUDO_UID/SUDO_GID), so a later run without root can update it.
void ab_give_back(const char *path);

// The USB disk as a block device (Linux: O_DIRECT).  ab_blk_open returns
// AB_ERR_PERM without read access, AB_ERR_UNSUPPORTED where not available.
// ab_blk_read: AB_OK, AB_ERR_IO (unreadable or short), AB_ERR_NODEV (device gone).
typedef struct ab_blk ab_blk;
int  ab_blk_open(const char *path, ab_blk **out);
int  ab_blk_read(ab_blk *b, uint64_t off, uint8_t *buf, size_t n);
void ab_blk_close(ab_blk *b);

// SHA-256 (FIPS 180-4), sha256.c
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } ab_sha256_ctx;
void ab_sha256_init(ab_sha256_ctx *c);
void ab_sha256_update(ab_sha256_ctx *c, const void *data, size_t n);
void ab_sha256_final(ab_sha256_ctx *c, uint8_t out[32]);
void ab_sha256(const void *data, size_t n, uint8_t out[32]);


static inline void ab_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void ab_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint16_t ab_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t ab_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#endif
