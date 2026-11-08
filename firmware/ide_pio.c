#include "ide_pio.h"
#include "ide.h"
#include "ataboy.pio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

static PIO pio = pio0;
static uint sm_read;
static uint sm_write;
static uint offset_read;
static uint offset_write;

volatile uint32_t ide_pio_faults = 0;

// A 256-word burst takes ~180-205 us at PIO mode 0 timing.  IORDY may stretch
// each strobe slightly; anything near this limit means IORDY is stuck low.
#define PIO_BURST_TIMEOUT_US 100000u

void ide_pio_init(void) {
    // Load both programs into PIO instruction memory (17 of 32 slots)
    offset_read  = pio_add_program(pio, &ide_read_program);
    offset_write = pio_add_program(pio, &ide_write_program);

    sm_read  = pio_claim_unused_sm(pio, true);
    sm_write = pio_claim_unused_sm(pio, true);

    // ---- Read SM (DIOR strobe) ----
    pio_sm_config c_rd = ide_read_program_get_default_config(offset_read);
    sm_config_set_in_pins(&c_rd, 0);                       // in base = GPIO 0
    sm_config_set_sideset_pins(&c_rd, 26);                  // side-set = DIOR
    sm_config_set_in_shift(&c_rd, false, true, 16);         // shift left, autopush @ 16
    sm_config_set_clkdiv(&c_rd, 15.0f);                    // 150 MHz / 15 = 10 MHz

    pio_sm_init(pio, sm_read, offset_read, &c_rd);

    // Set DIOR HIGH and output before the pin is handed to PIO (avoids LOW glitch)
    pio_sm_set_pins_with_mask(pio, sm_read, 1u << 26, 1u << 26);
    pio_sm_set_consecutive_pindirs(pio, sm_read, 26, 1, true);

    // ---- Write SM (DIOW strobe) ----
    pio_sm_config c_wr = ide_write_program_get_default_config(offset_write);
    sm_config_set_out_pins(&c_wr, 0, 16);                  // out base = GPIO 0, 16 pins
    sm_config_set_sideset_pins(&c_wr, 27);                  // side-set = DIOW
    sm_config_set_out_shift(&c_wr, true, false, 32);        // shift right, manual pull
    sm_config_set_clkdiv(&c_wr, 15.0f);

    pio_sm_init(pio, sm_write, offset_write, &c_wr);

    // Set DIOW HIGH and output; data bus starts as input
    pio_sm_set_pins_with_mask(pio, sm_write, 1u << 27, 1u << 27);
    pio_sm_set_consecutive_pindirs(pio, sm_write, 27, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm_write, 0, 16, false);

    // Only now hand GPIO 0-15 (data bus), 26 (DIOR) and 27 (DIOW) to PIO: the
    // state machines already drive both strobes high and leave the data bus
    // as inputs, so the switch from SIO (which holds the strobes high, see
    // ide_hw_init) causes no low glitch on DIOR-/DIOW-.
    for (int i = 0; i < 16; i++) pio_gpio_init(pio, i);
    pio_gpio_init(pio, 26);
    pio_gpio_init(pio, 27);

    // Start both SMs (they immediately stall on pull block)
    pio_sm_set_enabled(pio, sm_read, true);
    pio_sm_set_enabled(pio, sm_write, true);
}

// Put a wedged state machine back at the top of its program with its strobe
// deasserted, so the next burst starts clean.
static void pio_recover(uint sm, uint offset, uint strobe_pin) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset));
    pio_sm_set_pins_with_mask(pio, sm, 1u << strobe_pin, 1u << strobe_pin);
    pio->irq = 1u << sm;
    pio_sm_set_enabled(pio, sm, true);
    ide_pio_faults++;
}

bool ide_pio_read(uint32_t count, uint16_t *buf) {
    uint32_t t0 = time_us_32();
    // Push count-1 to start the read burst
    pio_sm_put_blocking(pio, sm_read, count - 1);

    for (uint32_t i = 0; i < count; i++) {
        while (pio_sm_is_rx_fifo_empty(pio, sm_read)) {
            if (time_us_32() - t0 > PIO_BURST_TIMEOUT_US) {
                pio_recover(sm_read, offset_read, 26);
                return false;
            }
        }
        buf[i] = (uint16_t)pio_sm_get(pio, sm_read);
    }

    // Wait for PIO completion IRQ, then clear it to release the SM
    while (!(pio->irq & (1u << sm_read))) {
        if (time_us_32() - t0 > PIO_BURST_TIMEOUT_US) {
            pio_recover(sm_read, offset_read, 26);
            return false;
        }
    }
    pio->irq = 1u << sm_read;
    return true;
}

bool ide_pio_write(uint32_t count, const uint16_t *buf) {
    uint32_t t0 = time_us_32();
    bool ok = true;

    // Data bus to output for the duration of the write
    pio_sm_set_consecutive_pindirs(pio, sm_write, 0, 16, true);

    // Push count-1, then each data word
    pio_sm_put_blocking(pio, sm_write, count - 1);
    for (uint32_t i = 0; i < count && ok; i++) {
        while (pio_sm_is_tx_fifo_full(pio, sm_write)) {
            if (time_us_32() - t0 > PIO_BURST_TIMEOUT_US) { ok = false; break; }
        }
        if (ok) pio_sm_put(pio, sm_write, (uint32_t)buf[i]);
    }

    // Wait for completion
    while (ok && !(pio->irq & (1u << sm_write))) {
        if (time_us_32() - t0 > PIO_BURST_TIMEOUT_US) ok = false;
    }
    if (ok) pio->irq = 1u << sm_write;
    else    pio_recover(sm_write, offset_write, 27);

    // Data bus back to input
    pio_sm_set_consecutive_pindirs(pio, sm_write, 0, 16, false);
    return ok;
}
