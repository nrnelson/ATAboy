#ifndef FAULT_H
#define FAULT_H

// HardFault handling.  On a fault (either core) the handler, running from
// RAM on a stack of its own:
//   1. releases the IDE bus through the GPIO output overrides, which neither
//      SIO nor PIO (nor the other core) can undo: transceivers disabled, data
//      lines inputs, DIOR-/DIOW-/CS0-/CS1-/RESET- negated, DIR = IDE -> Pico;
//   2. records PC, LR, xPSR and the fault status registers in RAM that a
//      watchdog reboot keeps (uninitialized section + magic + check word);
//   3. reboots through the watchdog.
// The next boot latches the record (fault_init) and the API reports it as
// INFO "last_fault".  A power cycle loses it.

#include <stdint.h>
#include <stdbool.h>
#include "json.h"

typedef struct {
    uint32_t magic;
    uint32_t core;        // 0 / 1
    uint32_t pc, lr, xpsr;  // from the stacked exception frame (0 if the frame was unusable)
    uint32_t sp;          // address of the stacked frame
    uint32_t exc_return;  // LR on handler entry
    uint32_t cfsr, hfsr, mmfar, bfar, sfsr, sfar;
    uint32_t uptime_ms;   // since the boot that faulted
    uint32_t check;
} fault_record_t;

// Call first thing in main(): takes the record left by a fault before this
// boot (if any) and clears it.
void fault_init(void);

// The fault that caused this boot, or NULL.
const fault_record_t *fault_last(void);

// "key": {...} or null.
void fault_json(json_t *j, const char *key);

// Deliberately fault (permanently undefined instruction) to test the handler.
void fault_test(void) __attribute__((noreturn));

#endif
