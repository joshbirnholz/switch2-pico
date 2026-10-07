#ifndef S2P_WIFI_AP_H
#define S2P_WIFI_AP_H

#include <stdbool.h>

// On-demand Wi-Fi access point that serves the configuration page at
// http://192.168.4.1 (any hostname works thanks to the captive DNS).
// It switches itself off after a period without page activity.

void wifi_ap_init(void);
void wifi_ap_start(void);
void wifi_ap_stop(void);
void wifi_ap_toggle(void);
// Stop after a short delay (lets an in-flight HTTP response finish).
void wifi_ap_stop_later(void);
bool wifi_ap_active(void);
void wifi_ap_task(void);
// Called by the HTTP server on every request to keep the AP alive.
void wifi_ap_touch(void);

#endif
