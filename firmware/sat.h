#ifndef SAT_H
#define SAT_H

#include <stdint.h>

// SCSI/ATA Translation: ATA PASS-THROUGH (12) 0xA1 and (16) 0x85, so standard
// tools work unmodified (smartctl -d sat, hdparm, sg_sat_identify, sg_raw).
// 'cap' is the usable buffer size, 'xfer_len' the host's transfer length.
int32_t sat_passthrough(uint8_t lun, const uint8_t *cdb, uint8_t *buf, uint32_t cap, uint32_t xfer_len);

#endif
