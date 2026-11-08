#ifndef API_H
#define API_H

#include <stdint.h>
#include <stdbool.h>

// Vendor SCSI command 0xC0: the ATAboy automation API (docs/API.md).
#define API_OPCODE 0xC0

int32_t api_handle(uint8_t lun, const uint8_t *cdb, uint8_t *buf, uint32_t cap);

// One-shot arming for DANGEROUS pass-through commands.
bool api_is_armed(void);
bool api_take_arm(void);

// Deferred reboot requested by the API; polled from the core 0 main loop.
void api_poll(void);

#endif
