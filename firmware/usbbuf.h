#ifndef USBBUF_H
#define USBBUF_H

#include <stdint.h>
#include <stdbool.h>

// Guard against executing a PIO data-out pass-through command without host
// data.  TinyUSB invokes tud_msc_scsi_cb() for a data-in/no-data CBW before
// any data phase and for a data-out CBW only after the data has arrived, but
// gives the callback no way to tell which.  After every command the class
// buffer is stamped with a tag; real host data replaces it, so a buffer that
// still carries the tag means nothing was received.
void usbbuf_note(uint8_t *buf);          // remember the class buffer address
void usbbuf_poison(void);                // stamp the tag (after a command completes)
bool usbbuf_has_host_data(const uint8_t *buf);

// Mass-storage command activity, for the API serial port: true from a
// command's first callback (usbbuf_note) to its completion (usbbuf_poison).
// *seq changes with every callback.
bool usbbuf_msc_active(uint32_t *seq);

#endif
