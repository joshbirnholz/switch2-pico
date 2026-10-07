// HTTP routes of the configuration page.
//
//   GET  /                     the single page UI (web/index.html)
//   GET  /api/status           live state (JSON)
//   GET  /api/settings         current settings (JSON)
//   POST /api/settings         update settings (application/x-www-form-urlencoded)
//   POST /api/action?do=...    forget, disconnect, rumble, gyrocal, gyroclear,
//                              usb_reconnect, reboot, bootloader, factory,
//                              wifi_off
//   GET  /api/log              firmware log (text)
//   anything else              redirect to / (captive portal)

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "log.h"
#include "mapping.h"
#include "procon.h"
#include "s2_link.h"
#include "platform.h"
#include "settings.h"
#include "usb_mode.h"
#include "version.h"
#include "web_api.h"

// The page itself is embedded only on boards that serve it over HTTP (the
// Pico's Wi-Fi); elsewhere these weak placeholders are used.
__attribute__((weak)) const uint8_t web_ui_data[1] = {0};
__attribute__((weak)) const size_t web_ui_size = 0;

// ---------------------------------------------------------------------------
// Small JSON writer
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t cap, len;
} jbuf_t;

static void jb_printf(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_printf(jbuf_t *j, const char *fmt, ...) {
    if (j->len >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) j->len += (size_t)n;
    if (j->len > j->cap) j->len = j->cap;
}

static void jb_str(jbuf_t *j, const char *s) {
    jb_printf(j, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') jb_printf(j, "\\%c", ch);
        else if (ch < 0x20) jb_printf(j, "\\u%04x", ch);
        else jb_printf(j, "%c", ch);
    }
    jb_printf(j, "\"");
}

static bool jb_init(jbuf_t *j, size_t cap) {
    j->buf = malloc(cap);
    j->cap = cap;
    j->len = 0;
    return j->buf != NULL;
}

static void respond_json(http_response_t *r, jbuf_t *j) {
    r->status = 200;
    r->content_type = "application/json";
    r->body = (const uint8_t *)j->buf;
    r->body_len = j->len;
    r->body_owned = true;
}

static void respond_text(http_response_t *r, int status, const char *text) {
    r->status = status;
    r->content_type = "text/plain";
    r->body = (const uint8_t *)text;
    r->body_len = strlen(text);
}

static void addr_str(char *out, const uint8_t a[6]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------
static void api_status(http_response_t *r) {
    jbuf_t j;
    if (!jb_init(&j, 3072)) return respond_text(r, 500, "oom");
    s2_link_info_t li;
    s2_link_get_info(&li);
    procon_status_t ps;
    usb_mode_get_status(&ps);
    char a[18];

    jb_printf(&j, "{\"version\":");
    jb_str(&j, S2P_VERSION);
    jb_printf(&j, ",\"platform\":\"%s\",\"wifi\":%s,\"usb_mode\":\"%s\"", platform_name(),
              platform_has_wifi() ? "true" : "false", usb_mode_name(usb_mode_active()));
    jb_printf(&j, ",\"link\":{\"state\":\"%s\"", s2_link_state_name(li.state));
    addr_str(a, li.addr);
    jb_printf(&j, ",\"addr\":\"%s\",\"pid\":%u,\"serial\":", a, li.pid);
    jb_str(&j, li.serial);
    jb_printf(&j,
              ",\"battery_mv\":%u,\"charge_state\":%u,\"report_hz\":%.0f,\"conn_interval_ms\":%.2f,"
              "\"mtu\":%u,\"paired_now\":%s,\"pairing_ok\":%s,\"gyro_range\":%u,\"rssi\":%d,"
              "\"reports\":%lu,\"rumble_packets\":%lu,\"gyro_cal_busy\":%s}",
              li.battery_mv, li.charge_state, (double)li.report_rate_hz, li.conn_interval * 1.25, li.mtu,
              li.paired_this_session ? "true" : "false", li.pairing_ok ? "true" : "false",
              li.gyro_range_detected, li.last_rssi, (unsigned long)li.reports,
              (unsigned long)li.rumble_packets, li.gyro_cal_busy ? "true" : "false");

    addr_str(a, g_settings.ctrl_addr);
    jb_printf(&j, ",\"bond\":{\"bonded\":%s,\"addr\":\"%s\",\"pid\":%u}", g_settings.bonded ? "true" : "false", a,
              g_settings.ctrl_pid);

    jb_printf(&j,
              ",\"usb\":{\"mounted\":%s,\"handshake\":%s,\"report_mode\":%u,\"imu\":%s,\"vibration\":%s,"
              "\"player_lights\":%u,\"reports\":%lu,\"rumble_frames\":%lu,\"subcommands\":%lu}",
              ps.usb_mounted ? "true" : "false", ps.handshake_done ? "true" : "false", ps.report_mode,
              ps.imu_enabled ? "true" : "false", ps.vibration_enabled ? "true" : "false", ps.player_lights,
              (unsigned long)ps.reports_sent, (unsigned long)ps.rumble_frames, (unsigned long)ps.subcommands);

    s2_input_t in;
    if (s2_link_get_input(&in, NULL)) {
        procon_input_t out;
        mapping_apply(&g_settings, s2_link_mapping_ctx(), &in, &out);
        jb_printf(&j,
                  ",\"input\":{\"raw_buttons\":%lu,\"raw_l\":[%u,%u],\"raw_r\":[%u,%u],\"raw_accel\":[%d,%d,%d],"
                  "\"raw_gyro\":[%d,%d,%d],\"triggers\":[%u,%u],\"out_buttons\":%lu,\"out_l\":[%u,%u],"
                  "\"out_r\":[%u,%u],\"out_accel\":[%d,%d,%d],\"out_gyro\":[%d,%d,%d]}",
                  (unsigned long)in.buttons, in.stick_l[0], in.stick_l[1], in.stick_r[0], in.stick_r[1],
                  in.accel[0], in.accel[1], in.accel[2], in.gyro[0], in.gyro[1], in.gyro[2], in.trigger_l,
                  in.trigger_r, (unsigned long)out.buttons, out.stick_l[0], out.stick_l[1], out.stick_r[0],
                  out.stick_r[1], out.accel[0], out.accel[1], out.accel[2], out.gyro[0], out.gyro[1],
                  out.gyro[2]);
    } else {
        jb_printf(&j, ",\"input\":null");
    }

    jb_printf(&j, ",\"log_total\":%lu}", (unsigned long)log_total_written());
    respond_json(r, &j);
}

// ---------------------------------------------------------------------------
// GET /api/settings
// ---------------------------------------------------------------------------
static void api_settings_get(http_response_t *r) {
    jbuf_t j;
    if (!jb_init(&j, 2048)) return respond_text(r, 500, "oom");
    const settings_t *s = &g_settings;
    jb_printf(&j, "{\"map\":{");
    for (int i = 0; i < IN_COUNT; i++) {
        jb_printf(&j, "%s\"%s\":%u", i ? "," : "", in_button_name((in_button_t)i), s->button_map[i]);
    }
    jb_printf(&j, "},\"outputs\":[");
    for (int i = 0; i < OUT_COUNT; i++) {
        jb_printf(&j, "%s", i ? "," : "");
        jb_str(&j, out_button_name((out_button_t)i));
    }
    jb_printf(&j,
              "],\"deadzone\":%u,\"outer\":%u,\"swap_sticks\":%u,\"gc_threshold\":%u,"
              "\"gyro_enabled\":%u,\"gyro_range\":%u,\"gyro_scale\":%u,\"accel_scale\":%u,"
              "\"gyro_bias\":[%d,%d,%d],"
              "\"rumble_enabled\":%u,\"rumble_strength\":%u,\"rumble_freq_mode\":%u,\"rumble_freq_slope\":%u,"
              "\"usb_interval\":%u,\"led_follow_host\":%u,"
              "\"quick_remap\":%u,\"usb_mode\":%u,\"usb_detach\":%u,\"usb_wakeup\":%u,\"webusb\":%u,\"hotkey\":%u,\"wifi_autostart\":%u,\"wifi_channel\":%u,"
              "\"wifi_ssid\":",
              s->stick_deadzone_pct, s->stick_outer_pct, s->swap_sticks, s->gc_trigger_threshold, s->gyro_enabled,
              s->gyro_range, s->gyro_scale_pct, s->accel_scale_pct, s->gyro_bias[0], s->gyro_bias[1],
              s->gyro_bias[2], s->rumble_enabled, s->rumble_strength_pct, s->rumble_freq_mode,
              s->rumble_freq_slope, s->usb_report_interval_ms,
              s->led_follow_host, !s->quick_remap_off, s->usb_mode, s->usb_detach_when_idle, s->usb_remote_wakeup, s->webusb_enabled, s->hotkey_enabled,
              s->wifi_autostart, s->wifi_channel);
    jb_str(&j, s->wifi_ssid);
    jb_printf(&j, ",\"wifi_has_pass\":%s}", s->wifi_pass[0] ? "true" : "false");
    respond_json(r, &j);
}

// ---------------------------------------------------------------------------
// POST /api/settings (urlencoded)
// ---------------------------------------------------------------------------
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

typedef struct {
    const char *key;
    void *field;
    uint8_t size;     // 1 or 2 bytes
    bool usb;         // changing it requires USB re-enumeration
} num_field_t;

static bool apply_kv(settings_t *s, const char *k, const char *v, bool *usb_reconnect) {
    if (strncmp(k, "map_", 4) == 0) {
        for (int i = 0; i < IN_COUNT; i++) {
            if (strcmp(k + 4, in_button_name((in_button_t)i)) == 0) {
                s->button_map[i] = (uint8_t)atoi(v);
                return true;
            }
        }
        return false;
    }
    if (strcmp(k, "quick_remap") == 0) {
        s->quick_remap_off = atoi(v) ? 0 : 1;
        return true;
    }
    if (strcmp(k, "wifi_ssid") == 0) {
        snprintf(s->wifi_ssid, sizeof s->wifi_ssid, "%s", v);
        return true;
    }
    if (strcmp(k, "wifi_pass") == 0) {
        snprintf(s->wifi_pass, sizeof s->wifi_pass, "%s", v);
        return true;
    }
    const num_field_t fields[] = {
        {"deadzone", &s->stick_deadzone_pct, 1, false},
        {"outer", &s->stick_outer_pct, 1, false},
        {"swap_sticks", &s->swap_sticks, 1, false},
        {"gc_threshold", &s->gc_trigger_threshold, 1, false},
        {"gyro_enabled", &s->gyro_enabled, 1, false},
        {"gyro_range", &s->gyro_range, 1, false},
        {"gyro_scale", &s->gyro_scale_pct, 2, false},
        {"accel_scale", &s->accel_scale_pct, 2, false},
        {"rumble_enabled", &s->rumble_enabled, 1, false},
        {"rumble_strength", &s->rumble_strength_pct, 1, false},
        {"rumble_freq_mode", &s->rumble_freq_mode, 1, false},
        {"rumble_freq_slope", &s->rumble_freq_slope, 1, false},
        {"usb_interval", &s->usb_report_interval_ms, 1, true},
        {"led_follow_host", &s->led_follow_host, 1, false},
        {"usb_detach", &s->usb_detach_when_idle, 1, false},
        {"usb_wakeup", &s->usb_remote_wakeup, 1, false},
        {"webusb", &s->webusb_enabled, 1, true},
        {"usb_mode", &s->usb_mode, 1, false},
        {"hotkey", &s->hotkey_enabled, 1, false},
        {"wifi_autostart", &s->wifi_autostart, 1, false},
        {"wifi_channel", &s->wifi_channel, 1, false},
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        if (strcmp(k, fields[i].key) != 0) continue;
        long n = strtol(v, NULL, 10);
        if (n < 0) n = 0;
        if (fields[i].size == 1) {
            uint8_t nv = (uint8_t)(n > 255 ? 255 : n);
            if (*(uint8_t *)fields[i].field != nv && fields[i].usb) *usb_reconnect = true;
            *(uint8_t *)fields[i].field = nv;
        } else {
            uint16_t nv = (uint16_t)(n > 65535 ? 65535 : n);
            if (*(uint16_t *)fields[i].field != nv && fields[i].usb) *usb_reconnect = true;
            *(uint16_t *)fields[i].field = nv;
        }
        return true;
    }
    return false;
}

static void api_settings_post(const http_request_t *req, http_response_t *r) {
    char *body = malloc(req->body_len + 1);
    if (!body) return respond_text(r, 500, "oom");
    memcpy(body, req->body, req->body_len);
    body[req->body_len] = 0;

    settings_t s = g_settings;
    bool usb = false;
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = 0;
        url_decode(pair);
        url_decode(eq + 1);
        if (!apply_kv(&s, pair, eq + 1, &usb)) LOG("web: unknown setting %s", pair);
    }
    free(body);
    settings_sanitize(&s);
    bool mode_changed = s.usb_mode != g_settings.usb_mode;
    g_settings = s;
    if (mode_changed) {
        // The host must see a different device: save and restart.
        settings_save_now();
        LOG("web: USB mode -> %s, restarting", usb_mode_name((usb_mode_t)s.usb_mode));
        app_request_reboot(false);
    } else {
        settings_save_later();
        LOG("web: settings updated%s", usb ? " (USB re-enumeration)" : "");
        if (usb) app_request_usb_reconnect();
    }
    api_settings_get(r);
}

// ---------------------------------------------------------------------------
// POST /api/action
// ---------------------------------------------------------------------------
static bool query_get(const char *q, const char *key, char *out, size_t out_len) {
    size_t kl = strlen(key);
    while (q && *q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t n = strcspn(v, "&");
            if (n >= out_len) n = out_len - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return false;
}

static void api_action(const http_request_t *req, http_response_t *r) {
    char what[24];
    if (!query_get(req->query, "do", what, sizeof what)) return respond_text(r, 400, "missing do=");
    LOG("web: action %s", what);
    if (!strcmp(what, "forget")) s2_link_forget();
    else if (!strcmp(what, "disconnect")) s2_link_disconnect();
    else if (!strcmp(what, "rumble")) s2_link_test_rumble();
    else if (!strcmp(what, "gyrocal")) s2_link_start_gyro_calibration();
    else if (!strcmp(what, "gyroclear")) {
        g_settings.gyro_bias[0] = g_settings.gyro_bias[1] = g_settings.gyro_bias[2] = 0;
        settings_save_later();
    } else if (!strcmp(what, "usb_reconnect")) app_request_usb_reconnect();
    else if (!strcmp(what, "reboot")) app_request_reboot(false);
    else if (!strcmp(what, "bootloader")) app_request_reboot(true);
    else if (!strcmp(what, "factory")) {
        settings_factory_reset();
        app_request_reboot(false);
    } else if (!strcmp(what, "wifi_off")) {
        app_wifi_stop();   // deferred until this response has been sent
    } else return respond_text(r, 400, "unknown action");
    respond_text(r, 200, "ok");
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------
void web_api_handle(const http_request_t *req, http_response_t *r) {
    bool get = strcmp(req->method, "GET") == 0;
    bool post = strcmp(req->method, "POST") == 0;

    if (get && (!strcmp(req->path, "/") || !strcmp(req->path, "/index.html"))) {
        if (!web_ui_size) return respond_text(r, 404, "Open https://joshbirnholz.github.io/switch2-pico/");
        r->status = 200;
        r->content_type = "text/html; charset=utf-8";
        r->body = web_ui_data;
        r->body_len = web_ui_size;
        return;
    }
    if (get && !strcmp(req->path, "/api/status")) return api_status(r);
    if (get && !strcmp(req->path, "/api/settings")) return api_settings_get(r);
    if (post && !strcmp(req->path, "/api/settings")) return api_settings_post(req, r);
    if (post && !strcmp(req->path, "/api/action")) return api_action(req, r);
    if (get && !strcmp(req->path, "/api/log")) {
        char *buf = malloc(8192);
        if (!buf) return respond_text(r, 500, "oom");
        r->status = 200;
        r->content_type = "text/plain; charset=utf-8";
        r->body_len = log_copy(buf, 8192);
        r->body = (const uint8_t *)buf;
        r->body_owned = true;
        return;
    }
    // Captive portal: send everything else to the UI.
    r->status = 302;
    r->extra_headers = "Location: http://192.168.4.1/\r\n";
    respond_text(r, 302, "");
}
