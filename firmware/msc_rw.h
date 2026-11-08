#ifndef MSC_RW_H
#define MSC_RW_H

// READ(10) / WRITE(10) data path of the USB disk, without TinyUSB or SDK
// dependencies so it can be unit-tested on the host (tests/test_rw.c).
//
// TinyUSB calls tud_msc_read10_cb / tud_msc_write10_cb once per chunk of at
// most CFG_TUD_MSC_EP_BUFSIZE bytes; a callback may consume fewer bytes and is
// then called again for the rest.  This module keeps a little state across the
// chunks of one SCSI command:
//   - a read that fails part way returns the good sectors before the bad one;
//     the call for the rest fails at the bad LBA from the remembered failure,
//     without sending the drive the same sector again;
//   - one time budget (MSC_RW_BUDGET_MS) covers all chunks of a command, so a
//     slow drive can't keep one host command busy past the host's timeout.
// Mount and write-protect checks stay in usb.c.

#include <stdint.h>
#include <stdbool.h>

// No new drive command is started for a READ(10)/WRITE(10) once this much
// time has passed since its first chunk; the command then fails at the
// current sector (sense_set_budget()).  The Linux SCSI timeout for /dev/sdX
// is 30 s by default; the margin covers the one drive command that may still
// be running when the budget runs out (8-sector chunks: up to 8 x the
// per-sector timeout, about 8 s at the default 1 s).
#define MSC_RW_BUDGET_MS        10000u

// A continuation chunk follows the previous one as soon as USB has moved the
// data (milliseconds).  A gap longer than this means the command was ended in
// a way that skipped TinyUSB's complete callback (BOT or bus reset), so the
// next callback starts a new command.
#define MSC_RW_CONTINUE_MS      2000u

int32_t msc_rw_read(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buf, uint32_t bufsize);
int32_t msc_rw_write(uint8_t lun, uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t bufsize);

// The SCSI command finished (TinyUSB complete callbacks); forget its state.
void    msc_rw_end(void);

// Provided by the platform (usb.c on the device, the test on the host).
uint32_t msc_rw_now_ms(void);

#endif
