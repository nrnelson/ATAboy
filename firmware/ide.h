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

// --- IDE Interface ---
void    ide_select_device(uint8_t base);   // 0xA0 = master, 0xB0 = slave
uint8_t ide_probe_devices(void);           // reset + scan master/slave, return dev_base or 0
void    ide_hw_init(void);
void    ide_reset_drive(void);
bool    ide_wait_until_ready(uint32_t timeout_ms);

void    ide_write_reg(uint8_t reg, uint8_t val);
uint8_t ide_read_reg(uint8_t reg);
uint8_t ide_read_alt_status(void);
void    ide_write_control(uint8_t val);
void    ide_set_iordy(bool enabled);

bool    ide_identify(uint16_t *buf);
bool    ide_set_geometry(uint8_t heads, uint8_t spt);
void    ide_drain_sector(void);

int32_t ide_read_sectors(uint32_t lba, uint32_t count, uint8_t *buf);

// --- Recovery additions ---
typedef struct {
    uint32_t lba;          // failing LBA
    uint8_t  cmd;          // ATA command (0x20/0x24 READ, 0x22/0x23 READ LONG)
    uint8_t  status;       // status register at failure
    uint8_t  error;        // error register at failure (0 if timed out)
    bool     timed_out;    // true = gave up waiting (drive still busy)
    uint32_t elapsed_ms;   // command start -> failure
    uint32_t timeout_ms;   // timeout in force
    uint8_t  tf_sn, tf_cl, tf_ch, tf_dh;   // address registers at failure
} ide_error_t;

extern volatile uint32_t ide_read_timeout_ms;
extern ide_error_t ide_last_error;
int32_t ide_read_long(uint32_t lba, bool retries, uint8_t ecc_len,
                      uint8_t *buf, uint8_t *st_end);
int32_t ide_write_sectors(uint32_t lba, uint32_t count, const uint8_t *buf);

// Read task file registers 1-7 into tf[1]..tf[7] (tf[0] unused).
void    ide_read_taskfile(uint8_t tf[8]);

// Issue a single-sector READ SECTORS to 'target' (LBA or cylinder depending
// on 'lba' flag), wait for completion, and drain the DRQ data.
// Returns the status register value after the operation.
uint8_t ide_seek_read_one(uint32_t target, bool lba);

#endif
