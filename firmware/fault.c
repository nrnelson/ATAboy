// HardFault handler: release the IDE bus, record the fault, reboot.  See fault.h.
//
// The SDK's default handler is a breakpoint, which without a debugger locks
// the core up with the bus as it was: CS0- low and DIOR- held by a stalled
// read, or the data lines still driven after a write.  Everything here runs
// from RAM (a fault during a flash save has no XIP) and uses only inline
// register access.

#include "fault.h"
#include "ide.h"
#include "pico/stdlib.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/timer.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include <stddef.h>
#include <string.h>

#define FAULT_MAGIC 0xFA017ED5u

// Survives a watchdog reboot: crt0 neither loads nor zeroes this section.
static fault_record_t __uninitialized_ram(fault_saved);

static fault_record_t fault_prev;
static bool fault_prev_valid;

// One small stack per core: the faulting stack may be the problem.
#define FAULT_STACK_WORDS 256
static uint32_t fault_stack[2][FAULT_STACK_WORDS] __attribute__((aligned(8)));
// Read by the handler's assembly; in RAM like the handler.
uint32_t *fault_stack_tops[2] = { &fault_stack[0][FAULT_STACK_WORDS], &fault_stack[1][FAULT_STACK_WORDS] };

static uint32_t __not_in_flash_func(record_check)(const fault_record_t *r) {
    const uint32_t *w = (const uint32_t *)r;
    uint32_t x = 0x9E3779B9u;
    for (uint32_t i = 0; i < offsetof(fault_record_t, check) / 4; i++) x = (x ^ w[i]) * 0x01000193u;
    return x;
}

static inline void __not_in_flash_func(pin_override)(uint pin, uint32_t outover, uint32_t oeover) {
    io_rw_32 *ctrl = &io_bank0_hw->io[pin].ctrl;
    *ctrl = (*ctrl & ~(IO_BANK0_GPIO0_CTRL_OUTOVER_BITS | IO_BANK0_GPIO0_CTRL_OEOVER_BITS)) |
            (outover << IO_BANK0_GPIO0_CTRL_OUTOVER_LSB) | (oeover << IO_BANK0_GPIO0_CTRL_OEOVER_LSB);
}

static void __not_in_flash_func(release_bus)(void) {
    const uint32_t hi = IO_BANK0_GPIO0_CTRL_OUTOVER_VALUE_HIGH, lo = IO_BANK0_GPIO0_CTRL_OUTOVER_VALUE_LOW;
    const uint32_t drive = IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_ENABLE, off = IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_DISABLE;
    // Transceivers off first, then nothing drives the data lines.
    pin_override(IDE_OE, hi, drive);
    pin_override(IDE_OE1, hi, drive);
    for (uint p = 0; p < 16; p++) pin_override(p, 0, off);
    pin_override(IDE_DIOR, hi, drive);
    pin_override(IDE_DIOW, hi, drive);
    pin_override(IDE_CS0, hi, drive);
    pin_override(IDE_CS1, hi, drive);
    pin_override(IDE_RESET, hi, drive);
    pin_override(IDE_DIR, lo, drive);
    pin_override(IDE_DIR1, lo, drive);
}

static void __not_in_flash_func(reboot_now)(void) {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    // Reset everything apart from the oscillators, as watchdog_reboot() does.
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    watchdog_hw->scratch[4] = 0;                    // normal boot, no bootrom reboot vector
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;) __asm volatile ("nop");
}

// Called from isr_hardfault on the fault stack with the stacked frame of the
// code that faulted (r0-r3, r12, lr, pc, xpsr) and the EXC_RETURN value.
void __not_in_flash_func(fault_handler_c)(uint32_t *frame, uint32_t exc_return) __attribute__((noreturn, used));
void __not_in_flash_func(fault_handler_c)(uint32_t *frame, uint32_t exc_return) {
    release_bus();

    fault_record_t *r = &fault_saved;
    uintptr_t f = (uintptr_t)frame;
    bool frame_ok = !(f & 3u) && f >= SRAM_BASE && f + 32u <= SRAM_END;
    r->core       = *(volatile uint32_t *)SIO_BASE;   // SIO CPUID
    r->lr         = frame_ok ? frame[5] : 0;
    r->pc         = frame_ok ? frame[6] : 0;
    r->xpsr       = frame_ok ? frame[7] : 0;
    r->sp         = (uint32_t)f;
    r->exc_return = exc_return;
    r->cfsr       = *(volatile uint32_t *)0xE000ED28u;
    r->hfsr       = *(volatile uint32_t *)0xE000ED2Cu;
    r->mmfar      = *(volatile uint32_t *)0xE000ED34u;
    r->bfar       = *(volatile uint32_t *)0xE000ED38u;
    r->sfsr       = *(volatile uint32_t *)0xE000EDE4u;
    r->sfar       = *(volatile uint32_t *)0xE000EDE8u;
    r->uptime_ms  = timer_hw->timerawl / 1000u;
    r->magic      = FAULT_MAGIC;
    r->check      = record_check(r);
    __dsb();

    reboot_now();
    __builtin_unreachable();
}

// Replaces the SDK's weak default (crt0.S: a breakpoint).
void __attribute__((naked)) __not_in_flash_func(isr_hardfault)(void) {
    __asm volatile (
        "tst   lr, #4              \n"
        "ite   eq                  \n"
        "mrseq r0, msp             \n"
        "mrsne r0, psp             \n"
        "mov   r1, lr              \n"
        "ldr   r2, =0xd0000000     \n"   // SIO CPUID: 0 or 1
        "ldr   r2, [r2]            \n"
        "ldr   r3, =fault_stack_tops \n"
        "ldr   r3, [r3, r2, lsl #2]\n"
        "movs  r2, #0              \n"   // stack limit off: the fault stack is elsewhere
        "msr   msplim, r2          \n"
        "mov   sp, r3              \n"
        "b     fault_handler_c     \n"
        ".ltorg                    \n");
}

void fault_init(void) {
    if (fault_saved.magic == FAULT_MAGIC && fault_saved.check == record_check(&fault_saved)) {
        fault_prev = fault_saved;
        fault_prev_valid = true;
    }
    memset(&fault_saved, 0, sizeof(fault_saved));
}

const fault_record_t *fault_last(void) { return fault_prev_valid ? &fault_prev : NULL; }

void fault_json(json_t *j, const char *key) {
    const fault_record_t *r = fault_last();
    if (!r) { j_null(j, key); return; }
    j_obj(j, key);
    j_uint(j, "core", r->core);
    j_uint(j, "pc", r->pc);
    j_uint(j, "lr", r->lr);
    j_uint(j, "xpsr", r->xpsr);
    j_uint(j, "sp", r->sp);
    j_uint(j, "exc_return", r->exc_return);
    j_uint(j, "cfsr", r->cfsr);
    j_uint(j, "hfsr", r->hfsr);
    j_uint(j, "mmfar", r->mmfar);
    j_uint(j, "bfar", r->bfar);
    j_uint(j, "sfsr", r->sfsr);
    j_uint(j, "sfar", r->sfar);
    j_uint(j, "uptime_ms", r->uptime_ms);
    j_end(j);
}

void fault_test(void) {
    __asm volatile ("udf #0");
    __builtin_unreachable();
}
