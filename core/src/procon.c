#include "procon.h"

#include <string.h>

#include "platform.h"

#include "log.h"
#include "settings.h"
#include "usb_hid.h"

#define STD_REPORT_LEN 63      // payload length of 0x21 / 0x30 / 0x81 reports
#define IMU_OFFSET 12

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static procon_input_t s_in;
static bool s_connected;
static uint16_t s_battery_mv;
static bool s_charging;

static uint8_t s_mac[6];             // big-endian, as displayed
static uint8_t s_colors[12] = {
    0x32, 0x32, 0x32,   // body
    0xE6, 0xE6, 0xE6,   // buttons
    0x32, 0x32, 0x32,   // left grip
    0x32, 0x32, 0x32,   // right grip
};

static procon_status_t s_status;
static s1_rumble_state_t s_rumble_l, s_rumble_r;
static uint32_t s_next_report;
static uint32_t s_mount_time;
static bool s_was_mounted;
static bool check_mount(void);

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t data[STD_REPORT_LEN];
} reply_t;

#define REPLY_QUEUE_LEN 8
static reply_t s_replies[REPLY_QUEUE_LEN];
static int s_reply_head, s_reply_count;

// ---------------------------------------------------------------------------
// Emulated SPI flash
// ---------------------------------------------------------------------------
static const uint8_t SPI_IMU_FACTORY_CAL[24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // accel origin x/y/z
    0x00, 0x40, 0x00, 0x40, 0x00, 0x40,   // accel sensitivity (0x4000: +-8 g -> 4096 LSB/g)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // gyro origin
    0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,   // gyro sensitivity (0x343B)
};

// 6-axis horizontal offsets followed by the left stick parameters (deadzone,
// range ratio...), values from a genuine Pro Controller.
static const uint8_t SPI_6080[24] = {
    0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F,
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
    0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
};
static const uint8_t SPI_6098[18] = {
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
    0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
};

static uint8_t s_stick_cal[18];

static void pack12x6(uint8_t out[9], const uint16_t v[6]) {
    for (int i = 0; i < 3; i++) {
        out[i * 3 + 0] = (uint8_t)(v[i * 2] & 0xFF);
        out[i * 3 + 1] = (uint8_t)(((v[i * 2] >> 8) & 0x0F) | ((v[i * 2 + 1] & 0x0F) << 4));
        out[i * 3 + 2] = (uint8_t)(v[i * 2 + 1] >> 4);
    }
}

static void build_stick_cal(void) {
    const uint16_t c = S1_STICK_CENTER, r = S1_STICK_RANGE;
    // Left:  X max-above, Y max-above, X center, Y center, X min-below, Y min-below
    const uint16_t left[6] = {r, r, c, c, r, r};
    // Right: X center, Y center, X min-below, Y min-below, X max-above, Y max-above
    const uint16_t right[6] = {c, c, r, r, r, r};
    pack12x6(s_stick_cal, left);
    pack12x6(s_stick_cal + 9, right);
}

static uint8_t spi_read_byte(uint32_t a) {
    if (a >= 0x6000 && a < 0x6010) return 0xFF;                   // serial number: none
    if (a == 0x6012) return 0x03;                                  // device type: Pro Controller
    if (a == 0x601B) return 0x01;                                  // colours present
    if (a >= 0x6020 && a < 0x6038) return SPI_IMU_FACTORY_CAL[a - 0x6020];
    if (a >= 0x603D && a < 0x604F) return s_stick_cal[a - 0x603D];
    if (a >= 0x6050 && a < 0x605C) return s_colors[a - 0x6050];
    if (a >= 0x6080 && a < 0x6098) return SPI_6080[a - 0x6080];
    if (a >= 0x6098 && a < 0x60AA) return SPI_6098[a - 0x6098];
    if (a >= 0x8000 && a < 0x8000 + SPI_USER_CAL_SIZE) return g_settings.spi_user_cal[a - 0x8000];
    return 0xFF;
}

static void spi_write(uint32_t addr, const uint8_t *data, int len) {
    bool changed = false;
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + (uint32_t)i;
        if (a >= 0x8000 && a < 0x8000 + SPI_USER_CAL_SIZE) {
            if (g_settings.spi_user_cal[a - 0x8000] != data[i]) {
                g_settings.spi_user_cal[a - 0x8000] = data[i];
                changed = true;
            }
        }
    }
    if (changed) {
        LOG("procon: host wrote user calibration @%04lx (%d bytes)", (unsigned long)addr, len);
        settings_save_later();
    }
}

static void spi_erase(uint32_t addr) {
    // Sector erase: only the user calibration area is writable.
    if (addr >= 0x8000 && addr < 0x9000) {
        memset(g_settings.spi_user_cal, 0xFF, sizeof g_settings.spi_user_cal);
        settings_save_later();
    }
}

// ---------------------------------------------------------------------------
// Report building
// ---------------------------------------------------------------------------
static uint8_t battery_byte(void) {
    uint8_t level;
    if (!s_connected || s_battery_mv == 0) {
        level = 4;
    } else if (s_battery_mv >= 3950) {
        level = 4;
    } else if (s_battery_mv >= 3750) {
        level = 3;
    } else if (s_battery_mv >= 3600) {
        level = 2;
    } else if (s_battery_mv >= 3450) {
        level = 1;
    } else {
        level = 0;
    }
    // bits 7..5 capacity (0..4), bit 4 charging, bits 2..1 connection type
    // (0 = Pro Controller / grip), bit 0 powered by host.
    return (uint8_t)((level << 5) | (s_charging ? 0x10 : 0x00));
}

static void build_prefix(uint8_t *p) {
    procon_input_t in = s_in;
    if (!s_connected) {
        memset(&in, 0, sizeof in);
        in.stick_l[0] = in.stick_l[1] = in.stick_r[0] = in.stick_r[1] = S1_STICK_CENTER;
    }
    uint32_t ms = platform_millis();
    p[0] = (uint8_t)(ms / 5);              // the timer ticks every 5 ms on real hardware
    p[1] = battery_byte();
    p[2] = (uint8_t)(in.buttons);
    p[3] = (uint8_t)(in.buttons >> 8);
    p[4] = (uint8_t)(in.buttons >> 16);
    mapping_pack_stick(in.stick_l, p + 5);
    mapping_pack_stick(in.stick_r, p + 8);
    p[11] = 0x09;                          // vibrator input report byte
}

static void build_imu(uint8_t *p) {
    int16_t a[3] = {0, 0, 0}, g[3] = {0, 0, 0};
    if (s_connected && s_status.imu_enabled) {
        memcpy(a, s_in.accel, sizeof a);
        memcpy(g, s_in.gyro, sizeof g);
    }
    // Three IMU samples per report; we only have the most recent one.
    for (int k = 0; k < 3; k++) {
        uint8_t *o = p + IMU_OFFSET + 12 * k;
        for (int i = 0; i < 3; i++) {
            o[2 * i] = (uint8_t)a[i];
            o[2 * i + 1] = (uint8_t)(a[i] >> 8);
            o[6 + 2 * i] = (uint8_t)g[i];
            o[6 + 2 * i + 1] = (uint8_t)(g[i] >> 8);
        }
    }
}

// ---------------------------------------------------------------------------
// Reply queue
// ---------------------------------------------------------------------------
static void queue_reply(uint8_t id, const uint8_t *data, int len) {
    if (s_reply_count >= REPLY_QUEUE_LEN) {
        LOG("procon: reply queue full, dropping 0x%02x", id);
        return;
    }
    reply_t *r = &s_replies[(s_reply_head + s_reply_count) % REPLY_QUEUE_LEN];
    r->id = id;
    if (len > STD_REPORT_LEN) len = STD_REPORT_LEN;
    memset(r->data, 0, sizeof r->data);
    memcpy(r->data, data, (size_t)len);
    r->len = STD_REPORT_LEN;   // always send full length (macOS drops short reports)
    s_reply_count++;
}

static void reply_subcommand(uint8_t ack, uint8_t sub, const uint8_t *data, int len) {
    uint8_t p[STD_REPORT_LEN];
    memset(p, 0, sizeof p);
    build_prefix(p);
    p[12] = ack;
    p[13] = sub;
    if (len > STD_REPORT_LEN - 14) len = STD_REPORT_LEN - 14;
    if (data && len > 0) memcpy(p + 14, data, (size_t)len);
    queue_reply(0x21, p, sizeof p);
}

// ---------------------------------------------------------------------------
// Rumble
// ---------------------------------------------------------------------------
static void handle_rumble(const uint8_t *r8) {
    rumble_sample_t l[3], r[3];
    int nl = s1_rumble_decode(&s_rumble_l, r8, l);
    int nr = s1_rumble_decode(&s_rumble_r, r8 + 4, r);
    s_status.rumble_frames++;
    procon_hook_rumble(l, nl, r, nr);
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// CRC-8 (poly 0x07) used by the NFC/IR MCU replies.
static uint8_t mcu_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static void set_report_mode(uint8_t mode) {
    if (mode == 0x3F) mode = 0x30;   // simple HID mode is not used over USB
    // 0x31 (NFC/IR MCU data) is not supported: like a wired Pro Controller
    // without its MCU in use, keep sending standard reports.
    if (mode == 0x31) mode = 0x30;
    if (mode != 0x30) return;
    if (s_status.report_mode != mode) LOG("procon: report mode 0x%02x", mode);
    s_status.report_mode = mode;
}

static void handle_subcommand(uint8_t sub, const uint8_t *a, int alen) {
    uint8_t d[35];
    memset(d, 0, sizeof d);
    s_status.subcommands++;

    switch (sub) {
    case 0x01: {  // manual Bluetooth pairing (used by a Switch console over USB)
        uint8_t type = alen >= 1 ? a[0] : 3;
        if (type == 1) {
            static const char name[] = "Pro Controller";
            d[0] = 0x01;
            for (int i = 0; i < 6; i++) d[1 + i] = s_mac[5 - i];
            d[7] = 0x00;
            d[8] = 0x25;
            d[9] = 0x08;
            memcpy(d + 10, name, sizeof name - 1);
            d[29] = 0x68;
        } else if (type == 2) {
            static const uint8_t ltk[16] = {0xE5, 0xC8, 0xE4, 0x92, 0x05, 0xFF, 0xC9, 0x8A,
                                            0x7D, 0xEA, 0x15, 0xF6, 0x19, 0xBA, 0x82, 0x13};
            d[0] = 0x02;
            memcpy(d + 1, ltk, sizeof ltk);
        } else {
            d[0] = 0x03;
        }
        reply_subcommand(0x81, sub, d, 31);
        break;
    }
    case 0x02:    // device info
        d[0] = 0x03;          // firmware 3.72 (0x0348)
        d[1] = 0x48;
        d[2] = 0x03;          // controller type: Pro Controller
        d[3] = 0x02;
        memcpy(d + 4, s_mac, 6);
        d[10] = 0x01;
        d[11] = 0x01;         // colours are stored in SPI
        reply_subcommand(0x82, sub, d, 12);
        break;
    case 0x03:    // set input report mode
        if (alen >= 1) set_report_mode(a[0]);
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    case 0x04:    // trigger buttons elapsed time
        d[0] = 0x00; d[1] = 0xCC; d[2] = 0x00; d[3] = 0xEE; d[4] = 0x00; d[5] = 0xFF;
        reply_subcommand(0x83, sub, d, 14);
        break;
    case 0x10: {  // SPI flash read
        if (alen < 5) {
            reply_subcommand(0x80, sub, NULL, 0);
            break;
        }
        uint32_t addr = rd32(a);
        uint8_t len = a[4];
        if (len > 0x1D) len = 0x1D;
        memcpy(d, a, 4);
        d[4] = len;
        for (int i = 0; i < len; i++) d[5 + i] = spi_read_byte(addr + (uint32_t)i);
        reply_subcommand(0x90, sub, d, 5 + len);
        break;
    }
    case 0x11: {  // SPI flash write
        if (alen >= 5) {
            int len = a[4];
            if (len > alen - 5) len = alen - 5;
            spi_write(rd32(a), a + 5, len);
        }
        d[0] = 0x00;
        reply_subcommand(0x80, sub, d, 1);
        break;
    }
    case 0x12:    // SPI sector erase
        if (alen >= 4) spi_erase(rd32(a));
        d[0] = 0x00;
        reply_subcommand(0x80, sub, d, 1);
        break;
    case 0x21: {  // set NFC/IR MCU configuration: answer like an idle MCU
        static const uint8_t base[8] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
        uint8_t cfg[34];
        memset(cfg, 0, sizeof cfg);
        memcpy(cfg, base, sizeof base);
        cfg[sizeof cfg - 1] = mcu_crc8(cfg, sizeof cfg - 1);
        reply_subcommand(0xA0, sub, cfg, sizeof cfg);
        break;
    }
    case 0x22:    // set NFC/IR MCU state
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    case 0x30:    // set player lights
        if (alen >= 1) {
            s_status.player_lights = a[0];
            procon_hook_player_lights(a[0]);
        }
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    case 0x31:    // get player lights
        d[0] = s_status.player_lights;
        reply_subcommand(0xB0, sub, d, 1);
        break;
    case 0x40:    // enable IMU
        s_status.imu_enabled = alen >= 1 && a[0] != 0;
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    case 0x48:    // enable vibration
        s_status.vibration_enabled = alen >= 1 && a[0] != 0;
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    case 0x50:    // get regulated voltage
        d[0] = 0x18;
        d[1] = 0x06;
        reply_subcommand(0xD0, sub, d, 2);
        break;
    default:      // 0x06 HCI state, 0x08 low power, 0x38 home light, 0x41 IMU sensitivity, ...
        reply_subcommand(0x80, sub, NULL, 0);
        break;
    }
}

// ---------------------------------------------------------------------------
// USB 0x80 commands (wired-only protocol)
// ---------------------------------------------------------------------------
static void handle_usb_command(uint8_t cmd) {
    uint8_t d[10] = {0};
    LOG("procon: host USB command 0x80 0x%02x", cmd);
    switch (cmd) {
    case 0x01:    // status: connection type + MAC (little-endian)
        d[0] = 0x01;
        d[1] = 0x00;
        d[2] = 0x03;
        for (int i = 0; i < 6; i++) d[3 + i] = s_mac[5 - i];
        queue_reply(0x81, d, 9);
        break;
    case 0x02:    // handshake
    case 0x03:    // switch to 3 Mbit
        d[0] = cmd;
        queue_reply(0x81, d, 1);
        break;
    case 0x04:    // force USB (no Bluetooth timeout): start streaming
        s_status.handshake_done = true;
        if (!s_status.report_mode) s_status.report_mode = 0x30;
        break;
    case 0x05:    // allow Bluetooth again
        s_status.handshake_done = false;
        break;
    case 0x06:    // reset
        s_status.report_mode = 0;
        break;
    default:
        break;
    }
}

void usb_hid_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id) {
    uint8_t id;
    const uint8_t *p;
    int n;
    if (len < 1) return;
    check_mount();
    if (via_control && control_report_id != 0 && buf[0] != control_report_id) {
        id = control_report_id;
        p = buf;
        n = len;
    } else {
        id = buf[0];
        p = buf + 1;
        n = len - 1;
    }

    static uint32_t s_logged;
    if (id != 0x80 && id != 0x10 && s_logged < 24) {
        // The first few host reports, to see what the host driver does.
        s_logged++;
        LOG("procon: host report 0x%02x len %u%s sub 0x%02x", id, (unsigned)n, via_control ? " (control)" : "",
            id == 0x01 && n >= 10 ? p[9] : 0);
    }

    switch (id) {
    case 0x80:
        if (n >= 1) handle_usb_command(p[0]);
        break;
    case 0x01:    // [timer][rumble x8][subcommand][args...]
        if (n >= 10) {
            handle_rumble(p + 1);
            handle_subcommand(p[9], p + 10, n - 10);
        }
        break;
    case 0x10:    // rumble only
        if (n >= 9) handle_rumble(p + 1);
        break;
    case 0x11:    // rumble + NFC/IR MCU request (MCU part ignored)
        if (n >= 9) handle_rumble(p + 1);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void procon_init(void) {
    memset(&s_status, 0, sizeof s_status);
    s1_rumble_reset(&s_rumble_l);
    s1_rumble_reset(&s_rumble_r);
    build_stick_cal();

    // Stable per-board MAC in Nintendo's 7C:BB:8A OUI.
    uint8_t id[PLATFORM_UNIQUE_ID_LEN];
    platform_unique_id(id);
    s_mac[0] = 0x7C;
    s_mac[1] = 0xBB;
    s_mac[2] = 0x8A;
    s_mac[3] = id[5];
    s_mac[4] = id[6];
    s_mac[5] = id[7];
    s_next_report = platform_millis();
}

void procon_set_input(const procon_input_t *in, bool connected, uint16_t battery_mv, bool charging) {
    if (in) s_in = *in;
    s_connected = connected;
    s_battery_mv = battery_mv;
    s_charging = charging;
}

void procon_set_colors(const uint8_t rgb[12]) {
    memcpy(s_colors, rgb, sizeof s_colors);
}

void procon_get_status(procon_status_t *out) {
    *out = s_status;
    out->usb_mounted = usb_hid_mounted();
}

static void on_mount_change(bool mounted) {
    s_reply_head = s_reply_count = 0;
    s_status.handshake_done = false;
    s_status.report_mode = 0;
    s_status.imu_enabled = false;
    s1_rumble_reset(&s_rumble_l);
    s1_rumble_reset(&s_rumble_r);
    if (mounted) {
        s_mount_time = platform_millis();
        LOG("procon: USB mounted");
    } else {
        LOG("procon: USB unmounted");
    }
}

// Called before handling any host report as well as from procon_task(): a
// host driver (Linux hid-nintendo) can send its handshake within
// milliseconds of enumeration, and the mount reset must not wipe the reply.
static bool check_mount(void) {
    bool mounted = usb_hid_mounted();
    if (mounted != s_was_mounted) {
        s_was_mounted = mounted;
        on_mount_change(mounted);
    }
    return mounted;
}

void procon_task(void) {
    if (!check_mount()) return;

    // Hosts without a Switch driver never select a report mode; after a short
    // grace period start streaming anyway, like a controller left alone.
    if (!s_status.report_mode && platform_millis() - s_mount_time > 3000) {
        s_status.report_mode = 0x30;
        LOG("procon: no host init seen, streaming 0x30 reports");
    }

    static uint32_t s_replies_logged;
    if (!usb_hid_ready()) return;

    if (s_reply_count > 0) {
        reply_t *r = &s_replies[s_reply_head];
        if (usb_hid_send(r->id, r->data, r->len)) {
            if (s_replies_logged < 24) {
                s_replies_logged++;
                LOG("procon: sent reply 0x%02x %02x %02x", r->id, r->data[0], r->id == 0x21 ? r->data[13] : 0);
            }
            s_reply_head = (s_reply_head + 1) % REPLY_QUEUE_LEN;
            s_reply_count--;
        }
        return;
    }

    if (!s_status.report_mode) return;
    if (!platform_time_reached(s_next_report)) return;
    s_next_report = platform_deadline_ms(g_settings.usb_report_interval_ms);

    uint8_t rpt[STD_REPORT_LEN];
    memset(rpt, 0, sizeof rpt);
    build_prefix(rpt);
    build_imu(rpt);
    if (usb_hid_send(0x30, rpt, STD_REPORT_LEN)) s_status.reports_sent++;
}
