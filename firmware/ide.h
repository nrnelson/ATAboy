#ifndef IDE_H
#define IDE_H

#include <stdint.h>
#include <stdbool.h>

// --- Pin Definitions ---
// Data bus: GPIO 0-15   (PIO-managed)
// DIOR:     GPIO 26     (PIO-managed)
// DIOW:     GPIO 27     (PIO-managed)
// All others: SIO-managed from C

#define IDE_DIR         16
#define IDE_DIR1        17
#define IDE_OE          18
#define IDE_OE1         19
#define IDE_A0          20
#define IDE_A1          21
#define IDE_A2          22
#define IDE_RESET       23
#define IDE_CS0         24
#define IDE_CS1         25
#define IDE_DIOR        26      // PIO side-set (read SM)
#define IDE_DIOW        27      // PIO side-set (write SM)
#define IDE_IORDY       29      // 74LVC2G125 Buffer 1 output (1Y) → GPIO 29
#define IDE_INTRQ       28      // 74LVC2G125 Buffer 2 output (2Y) → GPIO 28

#define DATA_MASK       0x0000FFFF
#define ADDR_MASK       ((1 << IDE_A0) | (1 << IDE_A1) | (1 << IDE_A2))

// ATA status register bits
#define ATA_ST_BSY   0x80
#define ATA_ST_DRDY  0x40
#define ATA_ST_DF    0x20
#define ATA_ST_DSC   0x10
#define ATA_ST_DRQ   0x08
#define ATA_ST_CORR  0x04
#define ATA_ST_IDX   0x02
#define ATA_ST_ERR   0x01

// --- Bus lock ---
// Core 0 (USB callbacks) and core 1 (menu) both drive the IDE bus.  Every
// public function below takes this recursive lock; callers that need several
// operations to be atomic (detect = probe + identify + geometry) can hold it
// across them.
void    ide_lock(void);
void    ide_unlock(void);

// --- Low-level interface (each call is one locked bus transaction) ---
void    ide_select_device(uint8_t base);   // 0xA0 = master, 0xB0 = slave
uint8_t ide_selected_device(void);
uint8_t ide_probe_devices(uint8_t only);   // reset + scan; only = 0 (both), 0xA0 or 0xB0
void    ide_hw_init(void);
bool    ide_reset_drive(void);             // RESET line + recalibrate + restore CHS geometry
int     ide_soft_reset(void);              // SRST + restore CHS geometry; IDE_RST_*
int     ide_soft_reset_wait(uint32_t wait_ms);   // same, bounded per stage (SRST, RESET fallback)
bool    ide_reset_drive_wait(uint32_t wait_ms);
bool    ide_wait_until_ready(uint32_t timeout_ms);

void    ide_write_reg(uint8_t reg, uint8_t val);
uint8_t ide_read_reg(uint8_t reg);
uint8_t ide_read_alt_status(void);
void    ide_write_control(uint8_t val);
void    ide_set_iordy(bool enabled);

bool    ide_identify(uint16_t *buf);
bool    ide_set_geometry(uint8_t heads, uint8_t spt);
void    ide_drain_sector(void);

// Read task file registers 1-7 into tf[1]..tf[7] (tf[0] unused).
void    ide_read_taskfile(uint8_t tf[8]);

// Issue a single-sector READ SECTORS to 'target' (LBA or cylinder depending
// on 'lba' flag), wait for completion, and drain the DRQ data.
// Returns the status register value after the operation.
uint8_t ide_seek_read_one(uint32_t target, bool lba);

// --- Resets ---
// After SRST or RESET- the firmware waits up to IDE_RESET_WAIT_MS (the 31 s
// ATA allows) for the configured device, re-sends INITIALIZE DEVICE
// PARAMETERS in CHS mode and checks it was accepted.  A soft reset that
// doesn't bring the drive back gets one hardware reset.
#define IDE_RESET_WAIT_MS 31000u
// Recovery after a timeout inside a command waits less (per stage: SRST,
// then the RESET-line fallback), to stay inside the host's SCSI timeout.
#define IDE_RECOVERY_WAIT_MS 5000u

#define IDE_RST_OK      0   // SRST: drive back (and geometry accepted)
#define IDE_RST_HW      1   // SRST failed, the RESET line brought it back (or explicit hard reset ok)
#define IDE_RST_FAILED -1   // drive not back even after the RESET line
#define IDE_RST_NONE   -2   // no reset since boot / clear

typedef struct {
    uint32_t soft_resets;          // SRST issued (recovery, API, pass-through)
    uint32_t soft_reset_failures;  // drive not ready / geometry refused after SRST
    uint32_t hw_resets;            // RESET line pulses (fallback or explicit; not the detect probe)
    uint32_t hw_reset_failures;    // drive not back even after RESET
    uint32_t geometry_failures;    // INITIALIZE DEVICE PARAMETERS refused after a reset
    uint32_t self_resets;          // CHS sentinel gone before a command: the drive reset itself
    uint32_t reinits;              // 91h re-sent before a command (self-reset or geometry lost)
    uint32_t reinit_failures;      // ...and refused: the command was not issued
    uint32_t last_reset_ms;        // duration of the last reset (incl. fallback)
    int8_t   last_reset;           // IDE_RST_* of the last reset
} ide_health_t;

extern ide_health_t ide_health;
void    ide_health_clear(void);

// CHS self-reset check (see ide.c): state for the API.
#define IDE_CHK_OFF          0   // not in CHS mode
#define IDE_CHK_IDLE         1   // CHS, nothing armed (no good command since the last reset/geometry)
#define IDE_CHK_ARMED        2   // sentinel in place, checked before the next command
#define IDE_CHK_LOST         3   // geometry lost: 91h goes out before the next command
#define IDE_CHK_UNSUPPORTED  4   // drive doesn't keep SC/SN: no check until the geometry is set again
#define IDE_CHK_NO_91H       5   // the drive never accepted 91h: it runs on its default translation
uint8_t ide_chs_check_state(void);

// --- Generic ATA command engine ---
typedef enum {
    IDE_PROTO_NONDATA = 0,
    IDE_PROTO_PIO_IN  = 1,
    IDE_PROTO_PIO_OUT = 2,
} ide_proto_t;

// ide_exec() results
#define IDE_OK          0
#define IDE_ATA_ERROR  -1   // drive set ERR (or DF)
#define IDE_TIMEOUT    -2   // drive still BSY / no DRQ when the timeout ran out
#define IDE_NOT_READY  -3   // drive never became ready to accept the command
#define IDE_BUS_FAULT  -4   // PIO burst timed out (IORDY stuck low)
#define IDE_PROTOCOL   -5   // drive's data phase didn't match the request (DRQ left over)

// Events (ide_cmd_t.events)
#define IDE_EV_HW_RESET      0x01   // the recovery SRST failed and the RESET line was pulsed
#define IDE_EV_RESET_FAILED  0x02   // the drive didn't come back from the recovery reset
#define IDE_EV_SELF_RESET    0x04   // CHS: the drive reset itself since the previous command
#define IDE_EV_REINIT        0x08   // CHS: 91h re-sent before the command
#define IDE_EV_REINIT_FAILED 0x10   // ...and refused; the command was not issued (IDE_NOT_READY)

// Flags
#define IDE_F_NEED_DRDY     0x01   // wait for DRDY=1 (not just BSY=0) before issuing
#define IDE_F_TAKE_ERR_DATA 0x02   // ERR+DRQ: transfer the block anyway (read-through-error)
#define IDE_F_NO_RECOVER    0x04   // don't SRST after a timeout / protocol error

typedef struct {
    // ---- request ----
    uint8_t  command;
    uint8_t  feature, count, lba_lo, lba_mid, lba_hi;     // current registers
    uint8_t  hob_feature, hob_count, hob_lba_lo, hob_lba_mid, hob_lba_hi;  // 48-bit "previous"
    uint8_t  device;          // bit 6 = LBA, bits 3-0 = head / LBA 27:24; DEV bit is supplied by ide.c
    bool     ext;             // write HOB registers first, read them back after (48-bit command)
    ide_proto_t proto;
    uint8_t  *buf;            // data in/out, sectors*512 (+ ecc_len) bytes
    uint32_t sectors;         // 512-byte blocks to transfer
    uint8_t  ecc_len;         // READ/WRITE LONG: extra 8-bit bytes after the last block
    uint8_t  flags;           // IDE_F_*
    uint32_t timeout_ms;      // per-block DRQ wait and completion wait (0 = 1000)
    uint32_t ready_ms;        // wait for drive ready before issuing (0 = 5000)

    // ---- result ----
    int      result;          // IDE_OK / IDE_*
    uint8_t  status, error;   // final status / error registers
    uint8_t  out_count, out_lba_lo, out_lba_mid, out_lba_hi, out_device;
    uint8_t  out_hob_count, out_hob_lba_lo, out_hob_lba_mid, out_hob_lba_hi;
    bool     tf_valid;        // registers above were read with BSY=0 (else garbage)
    bool     err_data;        // block 'done' was transferred even though ERR was set
    bool     recovered;       // an SRST was issued afterwards
    uint8_t  events;          // IDE_EV_* (resets and geometry repairs around this command)
    uint32_t done;            // blocks transferred without error
    uint32_t elapsed_us;      // command written -> finished
} ide_cmd_t;

int     ide_exec(ide_cmd_t *c);

// Fill address registers for 'count' sectors at 'lba' using the configured
// addressing (CHS / LBA28 / LBA48).  allow_ext=false forces CHS/LBA28 (for
// commands that have no EXT form).  Returns false if the address can't be
// expressed (no geometry, LBA out of range for 28-bit, count too big).
bool    ide_cmd_address(ide_cmd_t *c, uint32_t lba, uint32_t count, bool allow_ext);

// Decode the address a command stopped at (after an error) from its result
// registers, in the addressing mode it was issued with.  False if the
// registers are not valid.
bool    ide_cmd_result_lba(const ide_cmd_t *c, uint32_t *lba);

#endif
