#include "status_led.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "s2_link.h"
#include "settings.h"
#include "web/wifi_ap.h"

static int s_last = -1;

void status_led_task(void) {
    uint32_t t = to_ms_since_boot(get_absolute_time());
    bool on;
    if (s2_link_pairing_open()) {
        on = (t / 100) & 1;   // pairing window: fast blink (5 Hz)
    } else if (wifi_ap_active()) {
        uint32_t ph = t % 1200;
        on = ph < 100 || (ph >= 200 && ph < 300);
    } else {
        switch (s2_link_state()) {
        case S2_LINK_READY:
            on = true;
            break;
        case S2_LINK_CONNECTING:
        case S2_LINK_DISCOVERING:
        case S2_LINK_INITIALISING:
            on = (t / 60) & 1;
            break;
        case S2_LINK_SCANNING: {
            uint8_t stage, reason;
            uint32_t age;
            if (s2_link_last_failure(&stage, &reason, &age) && age < 60000) {
                // Diagnostic: N quick blinks = the stage where the last
                // connection attempt failed (see README), then a pause.
                uint32_t ph = t % 3000;
                on = ph < stage * 400u && (ph % 400) < 150;
            } else {
                on = g_settings.bonded ? (t % 2000) < 150 : ((t / 250) & 1);
            }
            break;
        }
        default:
            on = false;
            break;
        }
    }
    if ((int)on != s_last) {
        s_last = on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    }
}
