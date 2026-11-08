#ifndef SENSE_H
#define SENSE_H

// Sense data we hand back on the next REQUEST SENSE.  TinyUSB overwrites the
// sense key with "medium not present" when a READ10/WRITE10 callback fails,
// so detailed sense is staged here and restored in tud_msc_request_sense_cb().

#include <stdint.h>
#include <stdbool.h>
#include "drive.h"

// Fixed-format sense describing a failed READ10/WRITE10 (see docs/API.md).
void sense_set_media(uint8_t lun, const errlog_entry_t *e);
// Plain fixed-format sense.
void sense_set(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq);
// Descriptor-format sense with the SAT "ATA Status Return" descriptor.
void sense_set_ata(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq, const ide_cmd_t *c);

#endif
