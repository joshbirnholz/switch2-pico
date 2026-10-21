#include "bootsel.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/platform.h"

#include "log.h"

#define CS_PIN_INDEX 1

bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    // Reading the button takes the flash away: park core 1 (the headphone
    // audio encoder runs from flash) in RAM meanwhile, as flash writes do.
    // Not waiting forever: if core 1 has stopped answering, the main loop
    // (USB, Bluetooth) must keep going; the button reads as released then.
    bool core1 = multicore_lockout_victim_is_initialized(1);
    if (core1 && !multicore_lockout_start_timeout_us(2000)) {
        static bool logged;
        if (!logged) {
            logged = true;
            LOG("bootsel: core 1 not responding, button not read");
        }
        return false;
    }
    uint32_t flags = save_and_disable_interrupts();

    // Float the chip select so the button (pulling it low) can be read.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {
    }
#if PICO_RP2040
    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
#else
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#endif
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    if (core1) multicore_lockout_end_timeout_us(2000);
    return pressed;
}
