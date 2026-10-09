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
#include "battery.h"
#include "log.h"
#include "mapping.h"
#include "procon.h"
#include "s2_link.h"
#include "s2_transport.h"
#include "platform.h"
#include "settings.h"
#include "usb_mode.h"
#include "mode_select.h"
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
    if (!jb_init(&j, 6144)) return respond_text(r, 500, "oom");
    s2_link_info_t li;
    s2_link_get_info(&li);
    procon_status_t ps;
    usb_mode_get_status(&ps);
    char a[18];

    jb_printf(&j, "{\"version\":");
    jb_str(&j, S2P_VERSION);
    jb_printf(&j, ",\"platform\":\"%s\",\"wifi\":%s,\"usb_mode\":\"%s\"", platform_name(),
              platform_has_wifi() ? "true" : "false", usb_mode_name(usb_mode_active()));
    // The profile running now: the one in use for the controller type the
    // dongle started for (or switched to).
    jb_printf(&j, ",\"profile\":");
    jb_str(&j, settings_active(&g_settings, settings_boot_ctrl(&g_settings))->name);
    jb_printf(&j, ",\"usb_mouse\":%s,\"ctrl_type\":%u", usb_mode_has_mouse() ? "true" : "false",
              s2_link_ctrl_type());
    platform_supply_t sup;
    if (platform_supply(&sup)) {
        jb_printf(&j, ",\"supply\":{\"vdd\":%u,\"vbus\":%u,\"vdd_min\":%u,\"vbus_min\":%u}", sup.vdd, sup.vbus,
                  sup.vdd_min, sup.vbus_min);
    }
    jb_printf(&j, ",\"pairing\":{\"required\":%s,\"open\":%s,\"left_ms\":%lu}",
              g_settings.pair_button ? "true" : "false", s2_link_pairing_open() ? "true" : "false",
              (unsigned long)s2_link_pairing_left_ms());
    jb_printf(&j, ",\"link\":{\"state\":\"%s\"", s2_link_state_name(li.state));
    addr_str(a, li.addr);
    jb_printf(&j, ",\"addr\":\"%s\",\"pid\":%u,\"serial\":", a, li.pid);
    jb_str(&j, li.serial);
    jb_printf(&j,
              ",\"battery_mv\":%u,\"battery_pct\":%u,\"charging\":%s,\"charge_state\":%u,\"report_hz\":%.0f,\"conn_interval_ms\":%.2f,"
              "\"mtu\":%u,\"paired_now\":%s,\"pairing_ok\":%s,\"gyro_range\":%u,\"rssi\":%d,"
              "\"reports\":%lu,\"rumble_packets\":%lu,\"gyro_cal_busy\":%s,\"power_level\":%d}",
              li.battery_mv, battery_percent(), battery_charging() ? "true" : "false", li.charge_state, (double)li.report_rate_hz, li.conn_interval * 1.25, li.mtu,
              li.paired_this_session ? "true" : "false", li.pairing_ok ? "true" : "false",
              li.gyro_range_detected, li.last_rssi, (unsigned long)li.reports,
              (unsigned long)li.rumble_packets, li.gyro_cal_busy ? "true" : "false", li.power_level);
    // Each connected controller (two for a Joy-Con 2 pair).
    jb_printf(&j, ",\"links\":[");
    bool first = true;
    for (int i = 0; i < S2T_LINKS; i++) {
        s2_link_info_t k;
        if (!s2_link_get_link_info(i, &k)) continue;
        addr_str(a, k.addr);
        // Its own battery: the controller's level when it gave one, else from the voltage.
        unsigned pct = k.power_level >= 0 ? (unsigned)k.power_level * 100u / 9u : battery_mv_to_percent(k.battery_mv);
        jb_printf(&j, "%s{\"state\":\"%s\",\"addr\":\"%s\",\"pid\":%u,\"battery_mv\":%u,\"battery_pct\":%u,\"power_level\":%d,"
                      "\"charging\":%s,\"report_hz\":%.0f,\"rumble_packets\":%lu}",
                  first ? "" : ",", s2_link_state_name(k.state), a, k.pid, k.battery_mv, pct, k.power_level,
                  (k.power_info & 2) ? "true" : "false", (double)k.report_rate_hz, (unsigned long)k.rumble_packets);
        first = false;
    }
    jb_printf(&j, "]");

    addr_str(a, g_settings.ctrl_addr);
    jb_printf(&j, ",\"bond\":{\"bonded\":%s,\"addr\":\"%s\",\"pid\":%u,\"max\":%d,\"list\":[", g_settings.bonded ? "true" : "false", a,
              g_settings.ctrl_pid, BOND_MAX);
    for (int i = 0; i < BOND_MAX && g_settings.bonds[i].used; i++) {
        addr_str(a, g_settings.bonds[i].addr);
        jb_printf(&j, "%s{\"addr\":\"%s\",\"pid\":%u}", i ? "," : "", a, g_settings.bonds[i].pid);
    }
    jb_printf(&j, "]}");

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
    if (!jb_init(&j, 24576)) return respond_text(r, 500, "oom");
    const settings_t *s = &g_settings;
    // Every mode's output names (null: the mode lacks it), then per
    // controller type every mode's map, its defaults and the mode shortcut
    // buttons, so the page can switch between them before saving. Switch
    // Pro maps are out_button_t, the others gp_out_t.
    jb_printf(&j, "{\"modes\":[");
    for (int m = 0; m < USB_MODE_COUNT; m++) {
        bool sw = m == USB_MODE_SWITCH_PRO;
        jb_printf(&j, "%s{\"family\":\"%s\",\"outputs\":[", m ? "," : "", sw ? "switch" : "gp");
        int n_out = sw ? OUT_COUNT : GP_COUNT;
        for (int i = 0; i < n_out; i++) {
            jb_printf(&j, "%s", i ? "," : "");
            const char *name = sw ? out_button_name((out_button_t)i) : usb_mode_output_name((usb_mode_t)m, (gp_out_t)i);
            if (name) jb_str(&j, name);
            else jb_printf(&j, "null");
        }
        jb_printf(&j, "]}");
    }
    // Per controller type: its profiles, one per button in mode_slot_t order
    // (A, B, X, Y, Up, Down, Left, Right; null: none), the one in use and
    // every mode's default map.
    // Profiles carry the Joy-Con 2 mouse fields (prof_ keys take them; see
    // parse_profile()).
    jb_printf(&j, "],\"ctrl_type\":%u,\"last_ctrl\":%u,\"profile_max\":%d,\"profile_mouse\":true,\"types\":[",
              s2_link_ctrl_type(), s->last_ctrl, PROFILE_MAX);
    for (int t = 0; t < CTRL_TYPE_COUNT; t++) {
        const ctrl_profiles_t *c = settings_profiles(s, (ctrl_type_t)t);
        jb_printf(&j, "%s{\"name\":", t ? "," : "");
        jb_str(&j, ctrl_type_name((ctrl_type_t)t));
        // numbered: no shortcut buttons (a single Joy-Con 2), profiles 1..8.
        jb_printf(&j, ",\"numbered\":%s,\"active\":%u,\"profiles\":[",
                  profiles_on_buttons((ctrl_type_t)t) ? "false" : "true", c->active);
        for (int i = 0; i < PROFILE_MAX; i++) {
            const profile_t *p = &c->p[i];
            jb_printf(&j, "%s", i ? "," : "");
            if (!p->used) {
                jb_printf(&j, "null");
                continue;
            }
            jb_printf(&j, "{\"name\":");
            jb_str(&j, p->name);
            jb_printf(&j, ",\"mode\":%u,\"deadzone\":%u,\"outer\":%u,\"swap\":%u,\"threshold\":%u,\"rumble\":%u,"
                          "\"strength\":%u,\"mouse\":%u,\"mouse_speed\":%u,\"mouse_flags\":%u,\"map\":[",
                      p->usb_mode, p->stick_deadzone_pct, p->stick_outer_pct, p->swap_sticks, p->trigger_threshold,
                      p->rumble_enabled, p->rumble_strength_pct, p->mouse_src, p->mouse_speed_pct, p->mouse_flags);
            for (int k = 0; k < IN_COUNT; k++) jb_printf(&j, "%s%u", k ? "," : "", p->map[k]);
            jb_printf(&j, "]}");
        }
        jb_printf(&j, "],\"defaults\":[");
        for (int m = 0; m < USB_MODE_COUNT; m++) {
            profile_t d;
            settings_default_profile_for((ctrl_type_t)t, (usb_mode_t)m, &d);
            jb_printf(&j, "%s[", m ? "," : "");
            for (int k = 0; k < IN_COUNT; k++) jb_printf(&j, "%s%u", k ? "," : "", d.map[k]);
            jb_printf(&j, "]");
        }
        jb_printf(&j, "]}");
    }
    jb_printf(&j, "]");
    jb_printf(&j, ",\"inputs\":[");
    for (int i = 0; i < IN_COUNT; i++) {
        jb_printf(&j, "%s", i ? "," : "");
        jb_str(&j, in_button_name((in_button_t)i));
    }
    jb_printf(&j,
              "],\"gyro_enabled\":%u,\"gyro_range\":%u,\"gyro_scale\":%u,\"accel_scale\":%u,"
              "\"gyro_bias\":[%d,%d,%d],"
              "\"rumble_freq_mode\":%u,\"rumble_freq_slope\":%u,"
              "\"usb_interval\":%u,\"led_follow_host\":%u,"
              "\"quick_remap\":%u,\"usb_detach\":%u,\"usb_wakeup\":%u,\"webusb\":%u,\"hotkey\":%u,\"wifi_autostart\":%u,\"wifi_channel\":%u,"
              "\"wifi_ssid\":",
              s->gyro_enabled,
              s->gyro_range, s->gyro_scale_pct, s->accel_scale_pct, s->gyro_bias[0], s->gyro_bias[1],
              s->gyro_bias[2], s->rumble_freq_mode,
              s->rumble_freq_slope, s->usb_report_interval_ms,
              s->led_follow_host, !s->quick_remap_off, s->usb_detach_when_idle, s->usb_remote_wakeup, s->webusb_enabled, s->hotkey_enabled,
              s->wifi_autostart, s->wifi_channel);
    jb_str(&j, s->wifi_ssid);
    jb_printf(&j, ",\"wifi_has_pass\":%s,\"ble_tx_power\":%u,\"idle_disconnect\":%u,\"idle_minutes\":%u,"
                  "\"pair_button\":%u,\"joycon_single\":%u,\"sync_button\":%s}",
              s->wifi_pass[0] ? "true" : "false", s->ble_tx_power, !s->idle_disconnect_off, s->idle_minutes,
              s->pair_button, s->joycon_single, platform_has_sync_button() ? "true" : "false");
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

// Profile keys (controller type t, profile i):
//   prof_<t>_<i>=<mode>,<deadzone>,<outer>,<swap>,<threshold>,<rumble>,<strength>,<map x IN_COUNT>,
//                m<mouse>,<mouse speed>,<mouse flags>,<name>
//                (empty: no profile there; the "m" group, Joy-Con 2 mouse, may be left out)
//   active_<t>=<i>        (i: the button, mode_slot_t; a cleared profile in
//                          use is replaced by another, see profiles.c)
static int s_parse_type, s_parse_index;   // the profile parse_profile() reads

static bool parse_profile(const char *v, profile_t *p) {
    memset(p, 0, sizeof *p);
    if (!*v) return true;   // unused
    uint8_t num[7 + IN_COUNT];
    char *end;
    for (size_t k = 0; k < sizeof num; k++) {
        long n = strtol(v, &end, 10);
        if (end == v || *end != ',') return false;
        num[k] = (uint8_t)(n < 0 ? 0 : n > 255 ? 255 : n);
        v = end + 1;
    }
    p->used = 1;
    p->usb_mode = num[0];
    p->stick_deadzone_pct = num[1];
    p->stick_outer_pct = num[2];
    p->swap_sticks = num[3];
    p->trigger_threshold = num[4];
    p->rumble_enabled = num[5];
    p->rumble_strength_pct = num[6];
    memcpy(p->map, num + 7, IN_COUNT);
    // Mouse fields: "m<n>,<n>,<n>," exactly (an older page's name such as
    // "mario" is still a name).
    uint8_t mo[3];
    const char *w = v;
    bool group = *w == 'm';
    if (group) w++;
    for (size_t k = 0; group && k < sizeof mo; k++) {
        if (!isdigit((unsigned char)*w)) {
            group = false;
            break;
        }
        long n = strtol(w, &end, 10);
        if (*end != ',') {
            group = false;
            break;
        }
        mo[k] = (uint8_t)(n > 255 ? 255 : n);
        w = end + 1;
    }
    if (group) {
        v = w;
        p->mouse_src = mo[0];
        p->mouse_speed_pct = mo[1];
        p->mouse_flags = mo[2];
    } else {
        // An older page: the profile's mouse fields stay as they are.
        const profile_t *old = &settings_profiles(&g_settings, (ctrl_type_t)s_parse_type)->p[s_parse_index];
        if (old->used) {
            p->mouse_src = old->mouse_src;
            p->mouse_speed_pct = old->mouse_speed_pct;
            p->mouse_flags = old->mouse_flags;
        }
    }
    snprintf(p->name, sizeof p->name, "%s", v);
    return true;
}

static bool apply_kv(settings_t *s, const char *k, const char *v, bool *usb_reconnect) {
    int t, i, n = 0;
    if (sscanf(k, "prof_%d_%d%n", &t, &i, &n) == 2 && !k[n]) {
        if (t < 0 || t >= CTRL_TYPE_COUNT || i < 0 || i >= PROFILE_MAX) return false;
        s_parse_type = t;
        s_parse_index = i;
        return parse_profile(v, &settings_profiles(s, (ctrl_type_t)t)->p[i]);
    }
    if (sscanf(k, "active_%d%n", &t, &n) == 1 && !k[n]) {
        if (t < 0 || t >= CTRL_TYPE_COUNT) return false;
        settings_profiles(s, (ctrl_type_t)t)->active = (uint8_t)atoi(v);
        return true;
    }
    if (strcmp(k, "quick_remap") == 0) {
        s->quick_remap_off = atoi(v) ? 0 : 1;
        return true;
    }
    if (strcmp(k, "idle_disconnect") == 0) {
        s->idle_disconnect_off = atoi(v) ? 0 : 1;
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
        {"gyro_enabled", &s->gyro_enabled, 1, false},
        {"gyro_range", &s->gyro_range, 1, false},
        {"gyro_scale", &s->gyro_scale_pct, 2, false},
        {"accel_scale", &s->accel_scale_pct, 2, false},
        {"rumble_freq_mode", &s->rumble_freq_mode, 1, false},
        {"rumble_freq_slope", &s->rumble_freq_slope, 1, false},
        {"usb_interval", &s->usb_report_interval_ms, 1, true},
        {"led_follow_host", &s->led_follow_host, 1, false},
        {"usb_detach", &s->usb_detach_when_idle, 1, false},
        {"usb_wakeup", &s->usb_remote_wakeup, 1, false},
        {"hotkey", &s->hotkey_enabled, 1, false},
        {"ble_tx_power", &s->ble_tx_power, 1, false},
        {"idle_minutes", &s->idle_minutes, 1, false},
        {"pair_button", &s->pair_button, 1, false},
        {"joycon_single", &s->joycon_single, 1, false},
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

    static settings_t s;   // not on the loop task's 4 KB stack
    s = g_settings;
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
    bool mode_changed = settings_boot_usb_mode(&s) != usb_mode_active() || settings_boot_mouse(&s) != usb_mode_has_mouse();
    g_settings = s;
    if (mode_changed) {
        // The host must see a different device: save and restart.
        settings_save_now();
        LOG("web: USB mode -> %s%s, restarting", usb_mode_name((usb_mode_t)settings_boot_usb_mode(&s)),
            settings_boot_mouse(&s) ? " with the mouse" : "");
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

// "AA:BB:CC:DD:EE:FF" (as addr_str prints it) to bytes.
static bool parse_addr(const char *v, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        char h[3] = {v[0], v[0] ? v[1] : 0, 0};
        char *end;
        if (!h[0] || !h[1]) return false;
        out[i] = (uint8_t)strtoul(h, &end, 16);
        if (*end) return false;
        v += 2;
        if (i < 5) {
            if (*v != ':' && strncmp(v, "%3A", 3) != 0 && strncmp(v, "%3a", 3) != 0) return false;
            v += *v == ':' ? 1 : 3;
        }
    }
    return *v == 0;
}

static void api_action(const http_request_t *req, http_response_t *r) {
    char what[24];
    if (!query_get(req->query, "do", what, sizeof what)) return respond_text(r, 400, "missing do=");
    LOG("web: action %s", what);
    if (!strcmp(what, "forget")) {
        // addr=AA:BB:CC:DD:EE:FF forgets that controller; without it, all of them.
        char v[24];
        uint8_t addr[6];
        if (query_get(req->query, "addr", v, sizeof v)) {
            if (!parse_addr(v, addr)) return respond_text(r, 400, "bad addr");
            s2_link_forget(addr);
        } else {
            s2_link_forget(NULL);
        }
    }
    else if (!strcmp(what, "disconnect")) s2_link_disconnect();
    else if (!strcmp(what, "rumble")) s2_link_test_rumble();
    else if (!strcmp(what, "pair")) {
        // Open (or close) the pairing window; only with "pair only after Sync" on.
        if (!g_settings.pair_button) return respond_text(r, 400, "pairing is always open on this dongle");
        if (s2_link_pairing_open()) s2_link_stop_pairing();
        else s2_link_start_pairing(60000);
    }
    else if (!strcmp(what, "motors")) {
        // Classic rumble test: l / r = 0..255, for 600 ms.
        char v[8];
        int l = query_get(req->query, "l", v, sizeof v) ? atoi(v) : 0;
        int rr = query_get(req->query, "r", v, sizeof v) ? atoi(v) : 0;
        s2_link_test_motors((uint8_t)(l < 0 ? 0 : l > 255 ? 255 : l), (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr), 600);
    } else if (!strcmp(what, "sample")) {
        // Built-in vibration sample n (for finding which one is which).
        char v[8];
        int n = query_get(req->query, "n", v, sizeof v) ? atoi(v) : 1;
        if (n < 0 || n > 255) return respond_text(r, 400, "n out of range");
        s2_link_play_sample((uint8_t)n);
    } else if (!strcmp(what, "haptic")) {
        char v[16];
        if (!query_get(req->query, "effect", v, sizeof v)) return respond_text(r, 400, "missing effect=");
        int e = 0;
        while (e < S2_HAPTIC_COUNT && strcmp(v, s2_link_haptic_name((s2_haptic_t)e))) e++;
        if (e == S2_HAPTIC_COUNT) return respond_text(r, 400, "unknown effect");
        s2_link_haptic((s2_haptic_t)e);
    }
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
// Firmware update (see platform_fw_* in platform.h)
//   GET  /api/fw/info                  {"family":..,"base":..,"max":..}
//   POST /api/fw/begin?size=N&crc=C    start (CRC-32 of the raw image)
//   POST /api/fw/chunk?off=O           binary body, in order
//   POST /api/fw/end                   verify and mark pending; then reboot
// ---------------------------------------------------------------------------
static uint32_t query_u32(const char *q, const char *key) {
    char v[16];
    return query_get(q, key, v, sizeof v) ? (uint32_t)strtoul(v, NULL, 0) : 0;
}

static void api_fw(const http_request_t *req, http_response_t *r, bool get, bool post) {
    const char *op = req->path + 8;
    const char *err = "bad request";
    bool ok = false;
    if (get && !strcmp(op, "info")) {
        platform_fw_info_t fi;
        platform_fw_info(&fi);
        jbuf_t j;
        if (!jb_init(&j, 128)) return respond_text(r, 500, "oom");
        jb_printf(&j, "{\"family\":%lu,\"base\":%lu,\"max\":%lu}", (unsigned long)fi.uf2_family,
                  (unsigned long)fi.base, (unsigned long)fi.max_size);
        return respond_json(r, &j);
    }
    if (!post) return respond_text(r, 405, "POST");
    if (!strcmp(op, "begin")) {
        ok = platform_fw_begin(query_u32(req->query, "size"), query_u32(req->query, "crc"), &err);
        if (ok) LOG("fw: update started (%lu bytes)", (unsigned long)query_u32(req->query, "size"));
    } else if (!strcmp(op, "chunk")) {
        ok = platform_fw_write(query_u32(req->query, "off"), (const uint8_t *)req->body, (uint32_t)req->body_len, &err);
    } else if (!strcmp(op, "end")) {
        ok = platform_fw_finish(&err);
        LOG("fw: %s", ok ? "image verified, installs at the next restart" : err);
    }
    if (!ok && strcmp(op, "chunk")) LOG("fw: %s failed: %s", op, err);
    respond_text(r, ok ? 200 : 400, ok ? "ok" : err);
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
    if (!strncmp(req->path, "/api/fw/", 8)) return api_fw(req, r, get, post);
    if (get && !strcmp(req->path, "/api/log")) {
        // Saved log from before this boot (boards that keep one), then this boot's.
        static const char sep[] = "===== saved log from before this start is above =====\n";
        const size_t saved_max = 6144, live_max = 8192;
        char *buf = malloc(saved_max + sizeof sep + live_max);
        if (!buf) return respond_text(r, 500, "oom");
        size_t n = platform_saved_log(buf, saved_max);
        if (n) {
            memcpy(buf + n, sep, sizeof sep - 1);
            n += sizeof sep - 1;
        }
        n += log_copy(buf + n, live_max);
        r->status = 200;
        r->content_type = "text/plain; charset=utf-8";
        r->body_len = n;
        r->body = (const uint8_t *)buf;
        r->body_owned = true;
        return;
    }
    // Captive portal: send everything else to the UI.
    r->status = 302;
    r->extra_headers = "Location: http://192.168.4.1/\r\n";
    respond_text(r, 302, "");
}
