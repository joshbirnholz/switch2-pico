#include "wifi_ap.h"

#include "lwip/ip4_addr.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "dhcp_server.h"
#include "dns_server.h"
#include "http_server.h"
#include "log.h"
#include "s2_link.h"
#include "settings.h"

#define AP_IDLE_TIMEOUT_MS (10 * 60 * 1000)

static bool s_active;
static absolute_time_t s_idle_deadline;
static bool s_stop_pending;
static absolute_time_t s_stop_at;

void wifi_ap_init(void) {
    s_active = false;
}

bool wifi_ap_active(void) {
    return s_active;
}

void wifi_ap_touch(void) {
    s_idle_deadline = make_timeout_time_ms(AP_IDLE_TIMEOUT_MS);
}

void wifi_ap_start(void) {
    if (s_active) return;
    const char *pass = g_settings.wifi_pass[0] ? g_settings.wifi_pass : NULL;
    uint32_t auth = pass ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    s2_link_set_low_duty_scan(true);
    cyw43_wifi_ap_set_channel(&cyw43_state, g_settings.wifi_channel);
    cyw43_arch_enable_ap_mode(g_settings.wifi_ssid, pass, auth);

    ip4_addr_t ip, mask;
    IP4_ADDR(&ip, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    dhcp_server_start(&ip, &mask);
    dns_server_start(&ip);
    http_server_start();
    s_active = true;
    wifi_ap_touch();
    LOG("wifi: access point \"%s\" up, open http://192.168.4.1", g_settings.wifi_ssid);
}

void wifi_ap_stop(void) {
    if (!s_active) return;
    http_server_stop();
    dns_server_stop();
    dhcp_server_stop();
    cyw43_arch_disable_ap_mode();
    s2_link_set_low_duty_scan(false);
    s_active = false;
    LOG("wifi: access point down");
}

void wifi_ap_toggle(void) {
    if (s_active) wifi_ap_stop();
    else wifi_ap_start();
}

void wifi_ap_stop_later(void) {
    s_stop_pending = true;
    s_stop_at = make_timeout_time_ms(1000);
}

void wifi_ap_task(void) {
    if (s_stop_pending && time_reached(s_stop_at)) {
        s_stop_pending = false;
        wifi_ap_stop();
    }
    if (s_active && time_reached(s_idle_deadline)) {
        LOG("wifi: idle timeout");
        wifi_ap_stop();
    }
}
