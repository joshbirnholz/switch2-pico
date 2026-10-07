#ifndef S2P_APP_H
#define S2P_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Services provided by main.c to the web interface.

// Re-enumerate on USB (needed after changing descriptor related settings).
void app_request_usb_reconnect(void);
// Reboot, optionally into the UF2 bootloader for firmware updates.
void app_request_reboot(bool bootloader);
// Turn the configuration Wi-Fi off shortly (no-op on boards without Wi-Fi).
void app_wifi_stop(void);
// Raw Switch 2 buttons of the last report (for the live view).
uint32_t app_raw_buttons(void);

#ifdef __cplusplus
}
#endif

#endif
