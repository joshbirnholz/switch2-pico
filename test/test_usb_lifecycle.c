// Exercise the actual lifecycle and update handlers against a simulated host.
// Unused application/API sections are discarded at link time, keeping this
// test independent of the hardware drivers and unrelated route handlers.
#include <stdio.h>
#include <stdlib.h>

#include "../core/src/app_core.c"
#include "../core/src/web_api.c"

settings_t g_settings;
static bool audio_open, config_only, suspended;
static uint32_t now_ms;
static unsigned disconnects, connects, rebuilds, reboots, sleeps, writes;
static unsigned factory_resets, bt_disconnects;

uint32_t platform_millis(void) { return now_ms; }
bool platform_audio_active(void) { return audio_open; }
bool platform_usb_config_only_supported(void) { return true; }
void platform_usb_rebuild(void) { rebuilds++; }
void platform_reboot(bool bootloader) { reboots++; s_reboot = 0; }
bool tud_disconnect(void) { disconnects++; return true; }
bool tud_connect(void) { connects++; return true; }
bool tud_suspended(void) { return suspended; }
bool tud_mounted(void) { return true; }
bool tud_remote_wakeup(void) { return true; }
bool usb_mode_config_only(void) { return config_only; }
void usb_mode_set_config_only(bool on) { config_only = on; }
s2_link_state_t s2_link_state(void) { return S2_LINK_READY; }
void s2_link_let_controller_sleep(void) { sleeps++; }
void log_printf(const char *fmt, ...) {}
void s2_link_forget(const uint8_t *addr) {}
void s2_link_disconnect(void) { bt_disconnects++; }
void s2_link_test_rumble(void) {}
void s2_link_test_motors(uint8_t l, uint8_t r, uint16_t ms) {}
void s2_link_play_sample(uint8_t sample) {}
void s2_link_haptic(s2_haptic_t effect) {}
const char *s2_link_haptic_name(s2_haptic_t effect) { return "tick"; }
void s2_link_start_gyro_calibration(void) {}
bool s2_link_pairing_open(void) { return false; }
void s2_link_stop_pairing(void) {}
void s2_link_start_pairing(uint32_t ms) {}
void settings_save_later(void) {}
void settings_factory_reset(void) { factory_resets++; }
void app_wifi_stop(void) {}
void platform_fw_info(platform_fw_info_t *out) {
    *out = (platform_fw_info_t){.uf2_family = 1, .base = 0x10000000, .max_size = 1024};
}
bool platform_fw_begin(uint32_t size, uint32_t crc, const char **err) { writes++; return true; }
bool platform_fw_write(uint32_t off, const uint8_t *data, uint32_t len, const char **err) { writes++; return true; }
bool platform_fw_finish(const char **err) { writes++; return true; }

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    // An active speaker OR microphone is represented by the platform query.
    // A controller loss must leave the host's sound card present indefinitely.
    audio_open = true;
    for (now_ms = 100; now_ms < 20000; now_ms += 1000) usb_present_controller(false);
    CHECK(!config_only && disconnects == 0 && rebuilds == 0);
    // Once audio closes, use the normal disconnect grace period.
    audio_open = false;
    usb_present_controller(false);
    now_ms += USB_GONE_MS - 1;
    usb_present_controller(false);
    CHECK(!config_only && disconnects == 0);
    now_ms++;
    usb_present_controller(false);
    CHECK(config_only && disconnects == 1 && rebuilds == 1);
    now_ms += 300;
    maintenance_task();
    CHECK(connects == 1);
    // maintenance_task sees a connected controller and restores its USB device.
    maintenance_task();
    now_ms += 300;
    maintenance_task();
    CHECK(!config_only && connects == 2);

    // A descriptor reconnect requested internally must wait for audio to close.
    unsigned old_disconnects = disconnects, old_connects = connects;
    audio_open = true;
    app_request_usb_reconnect();
    now_ms += 10000;
    maintenance_task();
    CHECK(disconnects == old_disconnects && connects == old_connects);
    CHECK(s_usb_reconnect && s_usb_reconnect_waiting);
    audio_open = false;
    maintenance_task();
    CHECK(disconnects == old_disconnects + 1 && connects == old_connects);
    now_ms += 299;
    maintenance_task();
    CHECK(connects == old_connects);
    now_ms++;
    maintenance_task();
    CHECK(connects == old_connects + 1 && !s_usb_reconnect);

    // An audio stream opening after a reboot request must postpone the reboot.
    app_request_reboot(false);
    audio_open = true;
    now_ms += 1000;
    maintenance_task();
    CHECK(reboots == 0);
    audio_open = false;
    maintenance_task();
    CHECK(reboots == 1);

    // Playback/recording counts as controller activity, even without buttons.
    s2_input_t in = {0};
    g_settings.idle_minutes = 1;
    idle_reset(&in);
    audio_open = true;
    now_ms += 120000;
    idle_check(&in, 0, false);
    CHECK(sleeps == 0 && s_idle_since == now_ms);
    audio_open = false;
    now_ms += 59999;
    idle_check(&in, 0, false);
    CHECK(sleeps == 0);
    now_ms++;
    idle_check(&in, 0, false);
    CHECK(sleeps == 1);

    // Every firmware-writing operation must reject active audio before any
    // flash backend is touched. Information queries remain available.
    const char *paths[] = {"/api/fw/begin", "/api/fw/chunk", "/api/fw/end"};
    for (unsigned i = 0; i < 3; i++) {
        http_request_t req = {.method = "POST", .path = paths[i], .query = "size=512&crc=1&off=0", .body = "x", .body_len = 1};
        http_response_t resp = {0};
        audio_open = true;
        unsigned before = writes;
        api_fw(&req, &resp, false, true);
        CHECK(resp.status == 409 && writes == before);
        CHECK(strstr((const char *)resp.body, "Close headphone playback") != NULL);
        audio_open = false;
        api_fw(&req, &resp, false, true);
        CHECK(resp.status == 200 && writes == before + 1);
    }
    audio_open = true;
    http_request_t req = {.method = "GET", .path = "/api/fw/info", .query = ""};
    http_response_t resp = {0};
    api_fw(&req, &resp, true, false);
    CHECK(resp.status == 200);
    if (resp.body_owned) free((void *)resp.body);

    // API actions must reject before destructive side effects (factory reset
    // in particular), rather than merely delaying the eventual USB restart.
    const char *actions[] = {"do=reboot", "do=bootloader", "do=factory", "do=usb_reconnect"};
    for (unsigned i = 0; i < 4; i++) {
        req = (http_request_t){.method = "POST", .path = "/api/action", .query = actions[i]};
        resp = (http_response_t){0};
        api_action(&req, &resp);
        CHECK(resp.status == 409);
        CHECK(!s_reboot && !s_usb_reconnect && factory_resets == 0);
    }
    // Losing only Bluetooth is allowed; usb_present_controller() above is
    // responsible for retaining the audio endpoints until the host closes.
    req.query = "do=disconnect";
    api_action(&req, &resp);
    CHECK(resp.status == 200 && bt_disconnects == 1);
    audio_open = false;
    req.query = "do=reboot";
    api_action(&req, &resp);
    CHECK(resp.status == 200 && s_reboot == 1);

    printf("USB lifecycle: %u passed, %u failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
