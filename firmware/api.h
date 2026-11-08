#ifndef API_H
#define API_H

#include <stdint.h>
#include <stdbool.h>

// Vendor SCSI command 0xC0: the ATAboy automation API (docs/API.md).
#define API_OPCODE 0xC0

// Result code for a run-once request id reused with a different request
// (the codes 0..16 are in drive.h; 17 is left for the next drive result).
#define R_ID_CONFLICT 18

// Call once at boot, before USB starts: picks the random boot id.
void api_init(void);

// SCSI entry point: a malformed CDB sets sense and returns -1.
int32_t api_handle(uint8_t lun, const uint8_t *cdb, uint8_t *buf, uint32_t cap);

// Transport-neutral entry point (SCSI and the API serial port).  Returns the
// reply length, or -1 for a malformed request with sense[0..2] = sense key,
// ASC, ASCQ (what SCSI reports as CHECK CONDITION).
int32_t api_dispatch(const uint8_t *cdb, uint8_t *buf, uint32_t cap, uint8_t sense[3]);

// One-shot arming for DANGEROUS pass-through commands.
bool api_is_armed(void);
bool api_take_arm(void);

// Deferred reboot requested by the API, state-counter upkeep; polled from the
// core 0 main loop.
void api_poll(void);

#endif
