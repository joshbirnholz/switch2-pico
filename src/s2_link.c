#include "s2_link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/rand.h"
#include "pico/time.h"

#include "amiibo.h"
#include "log.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define CONNECT_TIMEOUT_MS      4000
#define GATT_PHASE_TIMEOUT_MS   5000
#define CMD_TIMEOUT_MS          700
#define CMD_RETRIES             2
#define RUMBLE_MIN_GAP_MS       8      // never write rumble faster than this
#define RUMBLE_HOLD_MS          12     // re-send the current rumble this often
#define RUMBLE_IDLE_STOP_MS     120    // host stopped sending -> silence
#define NFC_POLL_INTERVAL_MS    150
#define NFC_BUFFER_SIZE         768
#define GYRO_DETECT_MS          1500

// Connection parameters: 7.5 ms interval (the BLE minimum), no latency,
// 2 s supervision timeout.
#define CONN_INTERVAL           6
#define CONN_SUPERVISION        200

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef enum {
    GP_NONE,
    GP_SERVICES,
    GP_CHARACTERISTICS,
    GP_DESCRIPTORS,
    GP_CMD_CCCD,
    GP_INIT,             // command sequence running
    GP_RATE_DESC,
    GP_INPUT_CCCD,
    GP_READY,
} gatt_phase_t;

static s2_link_state_t s_state = S2_LINK_OFF;
static gatt_phase_t s_phase = GP_NONE;
static hci_con_handle_t s_con = HCI_CON_HANDLE_INVALID;
static bd_addr_t s_peer;
static bd_addr_type_t s_peer_type;
static uint16_t s_peer_pid;
static bool s_paused;              // host asleep: scan for wake-ups but don't connect
static absolute_time_t s_pause_quiet_until;
static absolute_time_t s_seen_hook_next;
static absolute_time_t s_phase_deadline;
static btstack_packet_callback_registration_t s_hci_cb;

static gatt_client_service_t s_service;
static bool s_have_service;
static gatt_client_characteristic_t s_ch_input, s_ch_cmd, s_ch_cmd_rsp, s_ch_vib;
static bool s_have_input, s_have_cmd, s_have_cmd_rsp, s_have_vib;
static uint16_t s_rate_desc_handle;
static gatt_client_notification_t s_notif_input, s_notif_cmd;

static s2_input_t s_input;
static uint32_t s_input_seq;
static mapping_ctx_t s_map;
static s2_link_info_t s_info;
static uint32_t s_rate_count;
static absolute_time_t s_rate_window;

static bool s_need_pairing;
static uint8_t s_pair_a1[16];

// Gyro range detection (see mapping.h)
static uint32_t s_imu_first_ts;
static absolute_time_t s_imu_first_time;
static int s_imu_samples;
static bool s_imu_detected;

// Gyro bias calibration
static bool s_cal_active;
static int32_t s_cal_sum[3];
static int s_cal_n;
static absolute_time_t s_cal_end;

// ---------------------------------------------------------------------------
// Command queue: one command in flight, matched to its response by id.
// ---------------------------------------------------------------------------
typedef void (*cmd_cb_t)(bool ok, const s2_response_t *rsp, uint32_t ctx);

typedef struct {
    uint8_t buf[96];
    uint8_t len;
    uint8_t cmd, sub;
    cmd_cb_t cb;
    uint32_t ctx;
    uint8_t tries;
} cmd_t;

#define CMD_QUEUE_LEN 12
static cmd_t s_cmdq[CMD_QUEUE_LEN];
static int s_cmd_head, s_cmd_count;
static bool s_cmd_in_flight;
static absolute_time_t s_cmd_deadline;

static void cmd_queue_reset(void) {
    s_cmd_head = s_cmd_count = 0;
    s_cmd_in_flight = false;
}

static bool cmd_submit(uint8_t cmd, uint8_t sub, const uint8_t *data, size_t len, cmd_cb_t cb, uint32_t ctx) {
    if (s_cmd_count >= CMD_QUEUE_LEN) {
        LOG("s2: command queue full (0x%02x/0x%02x)", cmd, sub);
        return false;
    }
    cmd_t *c = &s_cmdq[(s_cmd_head + s_cmd_count) % CMD_QUEUE_LEN];
    size_t n = s2_build_command(c->buf, sizeof c->buf, cmd, sub, data, len);
    if (!n) return false;
    c->len = (uint8_t)n;
    c->cmd = cmd;
    c->sub = sub;
    c->cb = cb;
    c->ctx = ctx;
    c->tries = 0;
    s_cmd_count++;
    return true;
}

static bool cmd_submit_raw(const uint8_t *buf, size_t n, cmd_cb_t cb, uint32_t ctx) {
    if (s_cmd_count >= CMD_QUEUE_LEN || n > sizeof s_cmdq[0].buf) return false;
    cmd_t *c = &s_cmdq[(s_cmd_head + s_cmd_count) % CMD_QUEUE_LEN];
    memcpy(c->buf, buf, n);
    c->len = (uint8_t)n;
    c->cmd = buf[0];
    c->sub = buf[3];
    c->cb = cb;
    c->ctx = ctx;
    c->tries = 0;
    s_cmd_count++;
    return true;
}

static void cmd_finish(bool ok, const s2_response_t *rsp) {
    cmd_t c = s_cmdq[s_cmd_head];
    s_cmd_head = (s_cmd_head + 1) % CMD_QUEUE_LEN;
    s_cmd_count--;
    s_cmd_in_flight = false;
    if (c.cb) c.cb(ok, rsp, c.ctx);
}

static void cmd_pump(void) {
    if (s_con == HCI_CON_HANDLE_INVALID || !s_have_cmd || s_cmd_count == 0) return;
    cmd_t *c = &s_cmdq[s_cmd_head];
    if (s_cmd_in_flight) {
        if (!time_reached(s_cmd_deadline)) return;
        if (c->tries <= CMD_RETRIES) {
            LOG("s2: command 0x%02x/0x%02x timed out, retrying", c->cmd, c->sub);
            s_cmd_in_flight = false;
        } else {
            LOG("s2: command 0x%02x/0x%02x failed", c->cmd, c->sub);
            cmd_finish(false, NULL);
            return;
        }
    }
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(s_con, s_ch_cmd.value_handle, c->len, c->buf);
    if (st == GATT_CLIENT_VALUE_TOO_LONG) {
        LOG("s2: command 0x%02x/0x%02x (%u bytes) exceeds the ATT MTU", c->cmd, c->sub, c->len);
        cmd_finish(false, NULL);
        return;
    }
    if (st != ERROR_CODE_SUCCESS) return;   // ACL buffers full: try again next pass
    c->tries++;
    s_cmd_in_flight = true;
    s_cmd_deadline = make_timeout_time_ms(CMD_TIMEOUT_MS);
}

static void cmd_on_response(const uint8_t *data, uint16_t len) {
    s2_response_t rsp;
    if (!s2_parse_response(data, len, &rsp)) return;
    if (!s_cmd_in_flight || s_cmd_count == 0) return;
    cmd_t *c = &s_cmdq[s_cmd_head];
    if (rsp.cmd != c->cmd || rsp.sub != c->sub) {
        LOG("s2: unexpected response 0x%02x/0x%02x", rsp.cmd, rsp.sub);
        return;
    }
    cmd_finish(true, &rsp);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void reverse16(uint8_t *dst, const uint8_t *src) {
    for (int i = 0; i < 16; i++) dst[i] = src[15 - i];
}

static void set_state(s2_link_state_t st) {
    if (st == s_state) return;
    bool was_ready = s_state == S2_LINK_READY;
    s_state = st;
    LOG("s2: state -> %s", s2_link_state_name(st));
    if (st == S2_LINK_READY) s2_link_hook_connection_changed(true);
    else if (was_ready) s2_link_hook_connection_changed(false);
}

const char *s2_link_state_name(s2_link_state_t st) {
    switch (st) {
    case S2_LINK_OFF: return "off";
    case S2_LINK_SCANNING: return "scanning";
    case S2_LINK_CONNECTING: return "connecting";
    case S2_LINK_DISCOVERING: return "discovering";
    case S2_LINK_INITIALISING: return "initialising";
    case S2_LINK_READY: return "ready";
    }
    return "?";
}

__attribute__((weak)) void s2_link_hook_controller_seen(void) {}
__attribute__((weak)) void s2_link_hook_connection_changed(bool connected) { (void)connected; }
__attribute__((weak)) void s2_link_hook_controller_colors(const uint8_t rgb[12]) { (void)rgb; }

static void start_scanning(void) {
    gap_set_scan_parameters(0, 0x0030, 0x0030);   // passive, 30 ms window every 30 ms
    gap_start_scan();
    set_state(S2_LINK_SCANNING);
}

static void reset_session(void) {
    if (s_con != HCI_CON_HANDLE_INVALID) {
        gatt_client_stop_listening_for_characteristic_value_updates(&s_notif_input);
        gatt_client_stop_listening_for_characteristic_value_updates(&s_notif_cmd);
    }
    s_con = HCI_CON_HANDLE_INVALID;
    s_phase = GP_NONE;
    s_have_service = s_have_input = s_have_cmd = s_have_cmd_rsp = s_have_vib = false;
    s_rate_desc_handle = 0;
    cmd_queue_reset();
    memset(&s_input, 0, sizeof s_input);
    s_imu_samples = 0;
    s_imu_detected = false;
    s_cal_active = false;
    s_info.nfc_active = false;
    s2_default_stick_cal(&s_map.cal_l);
    s2_default_stick_cal(&s_map.cal_r);
    s_map.is_gamecube = false;
    s_map.gc_trigger_neutral[0] = s_map.gc_trigger_neutral[1] = 30;
    s_map.gyro_lsb_per_dps = S2_GYRO_LSB_PER_DPS_A;
}

static void drop_connection(const char *why) {
    LOG("s2: dropping connection: %s", why);
    if (s_con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con);
}

// ---------------------------------------------------------------------------
// Rumble
// ---------------------------------------------------------------------------
static rumble_sample_t s_rum_l[3], s_rum_r[3];
static int s_rum_nl, s_rum_nr;
static bool s_rum_dirty;
static bool s_rum_active;
static uint32_t s_rum_last_host_ms;
static uint32_t s_rum_last_send_ms;
static uint8_t s_rum_seq;
// GameCube: no HD actuator; we play built-in vibration samples instead.
static float s_gc_last_mag;
static uint32_t s_gc_last_trigger_ms;

void s2_link_rumble_submit(const rumble_sample_t *left, int nl, const rumble_sample_t *right, int nr) {
    if (nl > 3) nl = 3;
    if (nr > 3) nr = 3;
    memcpy(s_rum_l, left, sizeof(rumble_sample_t) * (size_t)nl);
    memcpy(s_rum_r, right, sizeof(rumble_sample_t) * (size_t)nr);
    s_rum_nl = nl;
    s_rum_nr = nr;
    s_rum_dirty = true;
    s_rum_last_host_ms = now_ms();
}

static bool samples_active(const rumble_sample_t *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i].hi_amp > 0.0f || s[i].lo_amp > 0.0f) return true;
    }
    return false;
}

static void gc_rumble_task(uint32_t now) {
    float mag = 0.0f;
    for (int i = 0; i < s_rum_nl; i++) mag = fmaxf(mag, fmaxf(s_rum_l[i].hi_amp, s_rum_l[i].lo_amp));
    for (int i = 0; i < s_rum_nr; i++) mag = fmaxf(mag, fmaxf(s_rum_r[i].hi_amp, s_rum_r[i].lo_amp));
    mag *= (float)g_settings.rumble_strength_pct / 100.0f;
    if (now - s_rum_last_host_ms > RUMBLE_IDLE_STOP_MS) mag = 0.0f;
    bool rising = s_gc_last_mag < 0.06f && mag >= 0.06f;
    s_gc_last_mag = mag;
    if (mag < 0.06f) return;
    if (rising || now - s_gc_last_trigger_ms >= 220) {
        uint8_t d[4] = {(uint8_t)(mag >= 0.52f ? 0x02 : 0x03), 0, 0, 0};
        if (cmd_submit(S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0)) {
            s_gc_last_trigger_ms = now;
        }
    }
}

static void rumble_task(void) {
    if (s_state != S2_LINK_READY || !g_settings.rumble_enabled) return;
    uint32_t now = now_ms();
    if (s_map.is_gamecube) {
        gc_rumble_task(now);
        return;
    }
    if (!s_have_vib) return;

    bool host_idle = now - s_rum_last_host_ms > RUMBLE_IDLE_STOP_MS;
    bool want_active = !host_idle && (samples_active(s_rum_l, s_rum_nl) || samples_active(s_rum_r, s_rum_nr));

    bool send = false;
    if (s_rum_dirty && (want_active || s_rum_active)) send = true;
    if (want_active && now - s_rum_last_send_ms >= RUMBLE_HOLD_MS) send = true;
    if (!want_active && s_rum_active) send = true;        // final stop packet
    if (!send || now - s_rum_last_send_ms < RUMBLE_MIN_GAP_MS) return;

    s2_rumble_params_t p = {
        .translate_freq = g_settings.rumble_freq_mode == RUMBLE_FREQ_TRANSLATE,
        .freq_slope = g_settings.rumble_freq_slope,
        .strength_pct = g_settings.rumble_strength_pct,
    };
    uint8_t pkt[S2_RUMBLE_PRO_PACKET_LEN];
    pkt[0] = 0x00;
    if (want_active) {
        // A fresh host frame plays its own samples; a hold repeats the latest one.
        const rumble_sample_t *l = s_rum_dirty ? s_rum_l : &s_rum_l[s_rum_nl ? s_rum_nl - 1 : 0];
        const rumble_sample_t *r = s_rum_dirty ? s_rum_r : &s_rum_r[s_rum_nr ? s_rum_nr - 1 : 0];
        s2_rumble_encode_block(s_rum_seq, l, s_rum_dirty ? s_rum_nl : (s_rum_nl ? 1 : 0), &p, pkt + 1);
        s2_rumble_encode_block(s_rum_seq, r, s_rum_dirty ? s_rum_nr : (s_rum_nr ? 1 : 0), &p,
                               pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    } else {
        s2_rumble_encode_block(s_rum_seq, NULL, 0, &p, pkt + 1);
        s2_rumble_encode_block(s_rum_seq, NULL, 0, &p, pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    }
    uint16_t len = s_peer_pid == S2_PID_PRO2 ? S2_RUMBLE_PRO_PACKET_LEN : 1 + S2_RUMBLE_BLOCK_LEN;
    if (gatt_client_write_value_of_characteristic_without_response(s_con, s_ch_vib.value_handle, len, pkt) ==
        ERROR_CODE_SUCCESS) {
        s_rum_seq++;
        s_rum_dirty = false;
        s_rum_active = want_active;
        s_rum_last_send_ms = now;
        s_info.rumble_packets++;
    }
}

void s2_link_test_rumble(void) {
    if (s_state != S2_LINK_READY) return;
    uint8_t d[4] = {0x01, 0, 0, 0};   // built-in low frequency buzz
    cmd_submit(S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0);
}

// ---------------------------------------------------------------------------
// Player LEDs
// ---------------------------------------------------------------------------
static uint8_t s_led_wanted = 0x01;
static uint8_t s_led_sent = 0xFF;

void s2_link_set_player_leds(uint8_t pattern) {
    s_led_wanted = pattern & 0x0F;
}

static void led_task(void) {
    if (s_state != S2_LINK_READY || s_led_wanted == s_led_sent) return;
    uint8_t d[8] = {s_led_wanted, 0, 0, 0, 0, 0, 0, 0};
    if (cmd_submit(S2_CMD_LEDS, S2_SUB_LEDS_SET_PATTERN, d, sizeof d, NULL, 0)) s_led_sent = s_led_wanted;
}

// ---------------------------------------------------------------------------
// NFC (amiibo) reading
//
// The Switch 2 NFC commands mirror the Switch 1 MCU NFC protocol:
//   0x03 start polling, 0x04 stop, 0x05 status (state, uid), 0x06 read tag
//   into an internal buffer (Switch 1 style block list), 0x15 read that
//   buffer in chunks at a given offset.
// The response layouts were inferred from ndeadly's captures; the buffer is
// searched for the tag's UID/BCC bytes so the exact header size doesn't
// matter. Everything is logged to make field debugging possible.
// ---------------------------------------------------------------------------
typedef enum {
    NFCS_IDLE,
    NFCS_STARTING,
    NFCS_POLLING,
    NFCS_READ_ISSUED,
    NFCS_READING_BUFFER,
    NFCS_HAVE_TAG,
} nfc_phase_t;

static uint8_t s_nfc_sources;
static nfc_phase_t s_nfc_phase;
static bool s_nfc_cmd_pending;
static bool s_nfc_inited;
static absolute_time_t s_nfc_next;
static uint8_t s_nfc_buf[NFC_BUFFER_SIZE];
static uint16_t s_nfc_buf_len;
static uint16_t s_nfc_offset;
static int s_nfc_read_attempts;

void s2_link_nfc_request(nfc_source_t src, bool enable) {
    uint8_t before = s_nfc_sources;
    if (enable) s_nfc_sources |= (uint8_t)src;
    else s_nfc_sources &= (uint8_t)~src;
    if (before != s_nfc_sources) LOG("s2: NFC sources 0x%02x", s_nfc_sources);
}

static void nfc_cb(bool ok, const s2_response_t *rsp, uint32_t ctx);

static void nfc_send(uint8_t sub, const uint8_t *d, size_t n) {
    if (cmd_submit(S2_CMD_NFC, sub, d, n, nfc_cb, sub)) s_nfc_cmd_pending = true;
}

static void nfc_parse_status(const s2_response_t *rsp) {
    if (rsp->data_len < 9) return;
    const uint8_t *d = rsp->data;
    s_info.nfc_raw_state = d[0];
    uint8_t uid_len = d[8];
    bool present = d[4] != 0 && uid_len == NFC_UID_LEN && rsp->data_len >= 9u + uid_len;
    if (!present) {
        if (g_tag.state != TAG_NONE) {
            LOG("s2: NFC tag removed");
            amiibo_clear();
        }
        if (s_nfc_phase == NFCS_HAVE_TAG) s_nfc_phase = NFCS_POLLING;
        return;
    }
    const uint8_t *uid = d + 9;
    if (g_tag.state != TAG_NONE && memcmp(g_tag.uid, uid, NFC_UID_LEN) == 0) return;
    log_hex("s2: NFC tag", uid, NFC_UID_LEN);
    amiibo_set_uid(uid);
    g_tag.last_seen_ms = now_ms();
    s_nfc_read_attempts = 0;
    s_nfc_phase = NFCS_READ_ISSUED;
    // Read pages 0x00-0x3B, 0x3C-0x77, 0x78-0x86 of any tag (UID all zero), 2 s timeout.
    static const uint8_t read_args[19] = {0xd0, 0x07, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x03,
                                          0x00, 0x3b, 0x3c, 0x77, 0x78, 0x86, 0x00, 0x00};
    nfc_send(S2_SUB_NFC_READ_TAG, read_args, sizeof read_args);
}

static void nfc_try_complete(void) {
    int start = amiibo_find_pages(s_nfc_buf, s_nfc_buf_len, g_tag.uid);
    if (start >= 0 && start + NTAG215_SIZE <= s_nfc_buf_len) {
        LOG("s2: NFC tag read complete (pages at buffer offset %d)", start);
        amiibo_set_data(s_nfc_buf + start);
        s_nfc_phase = NFCS_HAVE_TAG;
        return;
    }
    if (++s_nfc_read_attempts < 4) {
        LOG("s2: NFC buffer incomplete (%u bytes, pages at %d), retrying", s_nfc_buf_len, start);
        s_nfc_phase = NFCS_READ_ISSUED;
        s_nfc_next = make_timeout_time_ms(300);
    } else {
        LOG("s2: NFC read failed");
        amiibo_set_failed();
        s_nfc_phase = NFCS_HAVE_TAG;
    }
}

static void nfc_cb(bool ok, const s2_response_t *rsp, uint32_t ctx) {
    s_nfc_cmd_pending = false;
    uint8_t sub = (uint8_t)ctx;
    if (!ok) {
        if (s_nfc_phase == NFCS_READING_BUFFER) nfc_try_complete();
        return;
    }
    switch (sub) {
    case S2_SUB_NFC_START_POLL:
        s_nfc_phase = NFCS_POLLING;
        break;
    case S2_SUB_NFC_STOP_POLL:
        s_nfc_phase = NFCS_IDLE;
        break;
    case S2_SUB_NFC_GET_STATUS:
        if (s_nfc_phase == NFCS_POLLING || s_nfc_phase == NFCS_HAVE_TAG) nfc_parse_status(rsp);
        break;
    case S2_SUB_NFC_READ_TAG:
        s_nfc_phase = NFCS_READING_BUFFER;
        s_nfc_buf_len = 0;
        s_nfc_offset = 0;
        s_nfc_next = make_timeout_time_ms(250);   // give the reader time to fill its buffer
        break;
    case S2_SUB_NFC_READ_BUFFER: {
        const uint8_t *d = rsp->data;
        size_t n = rsp->data_len;
        // Expected: [status][offset lo][offset hi][data...]
        if (n >= 3 && d[1] == (uint8_t)s_nfc_offset && d[2] == (uint8_t)(s_nfc_offset >> 8)) {
            d += 3;
            n -= 3;
        } else if (s_nfc_offset == 0) {
            log_hex("s2: NFC buffer header unexpected", rsp->data, rsp->data_len > 16 ? 16 : rsp->data_len);
        }
        if (n == 0) {
            nfc_try_complete();
            break;
        }
        if (s_nfc_buf_len + n > sizeof s_nfc_buf) n = sizeof s_nfc_buf - s_nfc_buf_len;
        memcpy(s_nfc_buf + s_nfc_buf_len, d, n);
        s_nfc_buf_len = (uint16_t)(s_nfc_buf_len + n);
        s_nfc_offset = (uint16_t)(s_nfc_offset + n);
        int start = amiibo_find_pages(s_nfc_buf, s_nfc_buf_len, g_tag.uid);
        if (s_nfc_buf_len >= sizeof s_nfc_buf || (start >= 0 && start + NTAG215_SIZE <= s_nfc_buf_len)) {
            nfc_try_complete();
        }
        break;
    }
    default:
        break;
    }
}

static void nfc_task(void) {
    bool want = s_nfc_sources != 0 && g_settings.nfc_enabled;
    if (s_state != S2_LINK_READY) {
        s_nfc_phase = NFCS_IDLE;
        s_nfc_cmd_pending = false;
        s_nfc_inited = false;
        return;
    }
    if (s_nfc_cmd_pending || !time_reached(s_nfc_next)) return;
    s_info.nfc_active = s_nfc_phase != NFCS_IDLE;

    if (!want) {
        if (s_nfc_phase != NFCS_IDLE && s_nfc_phase != NFCS_STARTING) {
            nfc_send(S2_SUB_NFC_STOP_POLL, NULL, 0);
            s_nfc_phase = NFCS_IDLE;
        }
        return;
    }

    switch (s_nfc_phase) {
    case NFCS_IDLE:
        if (!s_nfc_inited) {
            // Sent by SDL during its controller init; harmless if already active.
            cmd_submit(S2_CMD_NFC, S2_SUB_NFC_UNK0C, NULL, 0, NULL, 0);
            cmd_submit(S2_CMD_NFC, S2_SUB_NFC_INIT, NULL, 0, NULL, 0);
            s_nfc_inited = true;
        }
        {
            static const uint8_t poll_args[5] = {0x00, 0xe8, 0x03, 0x2c, 0x01};
            nfc_send(S2_SUB_NFC_START_POLL, poll_args, sizeof poll_args);
        }
        s_nfc_phase = NFCS_STARTING;
        break;
    case NFCS_STARTING:
        break;
    case NFCS_POLLING:
    case NFCS_HAVE_TAG:
        nfc_send(S2_SUB_NFC_GET_STATUS, NULL, 0);
        s_nfc_next = make_timeout_time_ms(NFC_POLL_INTERVAL_MS);
        break;
    case NFCS_READ_ISSUED:
        if (s_nfc_read_attempts > 0) {
            static const uint8_t read_args[19] = {0xd0, 0x07, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x03,
                                                  0x00, 0x3b, 0x3c, 0x77, 0x78, 0x86, 0x00, 0x00};
            nfc_send(S2_SUB_NFC_READ_TAG, read_args, sizeof read_args);
        }
        break;
    case NFCS_READING_BUFFER: {
        uint8_t off[2] = {(uint8_t)s_nfc_offset, (uint8_t)(s_nfc_offset >> 8)};
        nfc_send(S2_SUB_NFC_READ_BUFFER, off, sizeof off);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Initialisation sequence (after GATT discovery)
// ---------------------------------------------------------------------------
enum {
    ST_UNK07,
    ST_READ_INFO,
    ST_UNK16,
    ST_PAIR_ADDR,
    ST_PAIR_KEY,
    ST_PAIR_CONFIRM,
    ST_PAIR_FINALIZE,
    ST_VIB_SAMPLE,
    ST_LEDS,
    ST_FEATURE_MASK,
    ST_READ_CAL_L,
    ST_READ_CAL_R,
    ST_READ_USER_L,
    ST_READ_USER_R,
    ST_READ_GC_TRIGGERS,
    ST_VIB_DATA,
    ST_FEATURE_ENABLE,
    ST_DONE,
};

#define FEATURE_FLAGS (S2_FEATURE_BUTTONS | S2_FEATURE_STICKS | S2_FEATURE_IMU | S2_FEATURE_UNK08 | S2_FEATURE_RUMBLE)

static int s_init_step;
static uint8_t s_init_tries;
static void init_run(void);
static void after_init(void);

static void init_step_cb(bool ok, const s2_response_t *rsp, uint32_t step) {
    if ((int)step != s_init_step || s_phase != GP_INIT) return;
    if (!ok) {
        if (++s_init_tries <= 1) {
            init_run();
            return;
        }
        if (step == ST_FEATURE_MASK || step == ST_FEATURE_ENABLE) {
            drop_connection("controller did not accept feature configuration");
            return;
        }
        if (step >= ST_PAIR_ADDR && step <= ST_PAIR_FINALIZE) {
            LOG("s2: pairing step %lu failed; continuing unpaired", (unsigned long)step);
            s_info.pairing_ok = false;
            s_init_step = ST_VIB_SAMPLE;
            s_init_tries = 0;
            init_run();
            return;
        }
        s_init_step++;
        s_init_tries = 0;
        init_run();
        return;
    }

    const uint8_t *payload;
    switch (step) {
    case ST_READ_INFO:
        if (s2_parse_memory_read(rsp, S2_ADDR_DEVICE_INFO, 0x40, &payload)) {
            memcpy(s_info.serial, payload + 2, 14);
            s_info.serial[14] = 0;
            uint16_t pid = (uint16_t)(payload[0x14] | (payload[0x15] << 8));
            if (pid) s_peer_pid = pid;
            s_info.pid = s_peer_pid;
            s_map.is_gamecube = s_peer_pid == S2_PID_GAMECUBE;
            s2_link_hook_controller_colors(payload + 0x19);
            LOG("s2: controller pid=%04x serial=%s", s_peer_pid, s_info.serial);
        }
        break;
    case ST_PAIR_ADDR:
        log_hex("s2: pair address reply", rsp->data, rsp->data_len > 9 ? 9 : rsp->data_len);
        break;
    case ST_PAIR_KEY:
        if (rsp->data_len >= 17) {
            uint8_t b1[16];
            reverse16(b1, rsp->data + 1);
            for (int i = 0; i < 16; i++) g_settings.ltk[i] = s_pair_a1[i] ^ b1[i];
        }
        break;
    case ST_PAIR_FINALIZE:
        LOG("s2: pairing complete");
        s_info.pairing_ok = true;
        s_info.paired_this_session = true;
        g_settings.bonded = 1;
        memcpy(g_settings.ctrl_addr, s_peer, 6);
        g_settings.ctrl_addr_type = (uint8_t)s_peer_type;
        g_settings.ctrl_pid = s_peer_pid;
        settings_save_later();
        break;
    case ST_READ_CAL_L:
        if (s2_parse_memory_read(rsp, S2_ADDR_FACTORY_STICK_L, 0x40, &payload)) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 0x28, &c);
            if (c.valid) s_map.cal_l = c;
        }
        break;
    case ST_READ_CAL_R:
        if (s2_parse_memory_read(rsp, S2_ADDR_FACTORY_STICK_R, 0x40, &payload)) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 0x28, &c);
            if (c.valid) s_map.cal_r = c;
        }
        break;
    case ST_READ_USER_L:
        if (s2_parse_memory_read(rsp, S2_ADDR_USER_STICK_L, 0x40, &payload) && payload[0] == 0xb2 &&
            payload[1] == 0xa1) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 2, &c);
            if (c.valid) s_map.cal_l = c;
        }
        break;
    case ST_READ_USER_R:
        if (s2_parse_memory_read(rsp, S2_ADDR_USER_STICK_R, 0x40, &payload) && payload[0] == 0xb2 &&
            payload[1] == 0xa1) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 2, &c);
            if (c.valid) s_map.cal_r = c;
        }
        LOG("s2: stick cal L c=%u,%u +%u,%u -%u,%u  R c=%u,%u +%u,%u -%u,%u",
            s_map.cal_l.center[0], s_map.cal_l.center[1], s_map.cal_l.max[0], s_map.cal_l.max[1],
            s_map.cal_l.min[0], s_map.cal_l.min[1], s_map.cal_r.center[0], s_map.cal_r.center[1],
            s_map.cal_r.max[0], s_map.cal_r.max[1], s_map.cal_r.min[0], s_map.cal_r.min[1]);
        break;
    case ST_READ_GC_TRIGGERS:
        if (s2_parse_memory_read(rsp, S2_ADDR_GC_TRIGGERS, 0x02, &payload) && payload[0] != 0x00 &&
            payload[0] != 0xFF) {
            s_map.gc_trigger_neutral[0] = payload[0];
            s_map.gc_trigger_neutral[1] = payload[1];
        }
        break;
    case ST_LEDS:
        s_led_sent = s_led_wanted;
        break;
    default:
        break;
    }
    s_init_step++;
    s_init_tries = 0;
    init_run();
}

static void init_submit(uint8_t cmd, uint8_t sub, const uint8_t *d, size_t n) {
    if (!cmd_submit(cmd, sub, d, n, init_step_cb, (uint32_t)s_init_step)) {
        drop_connection("command queue");
    }
}

static void init_submit_read(uint32_t addr, uint8_t len) {
    uint8_t buf[32];
    size_t n = s2_build_memory_read(buf, sizeof buf, addr, len);
    if (!cmd_submit_raw(buf, n, init_step_cb, (uint32_t)s_init_step)) drop_connection("command queue");
}

static void init_run(void) {
    for (;;) {
        switch (s_init_step) {
        case ST_UNK07:
            init_submit(S2_CMD_UNK07, 0x01, NULL, 0);
            return;
        case ST_READ_INFO:
            init_submit_read(S2_ADDR_DEVICE_INFO, 0x40);
            return;
        case ST_UNK16:
            init_submit(S2_CMD_UNK16, 0x01, NULL, 0);
            return;
        case ST_PAIR_ADDR: {
            if (!s_need_pairing) {
                s_init_step = ST_VIB_SAMPLE;
                continue;
            }
            bd_addr_t local;
            gap_local_bd_addr(local);
            uint8_t d[14];
            d[0] = 0x00;
            d[1] = 0x02;
            for (int i = 0; i < 6; i++) d[2 + i] = local[5 - i];
            memcpy(d + 8, d + 2, 6);
            LOG("s2: pairing with host address %s", bd_addr_to_str(local));
            init_submit(S2_CMD_PAIR, S2_SUB_PAIR_SET_ADDRESS, d, sizeof d);
            return;
        }
        case ST_PAIR_KEY: {
            uint8_t d[17];
            for (int i = 0; i < 16; i += 4) {
                uint32_t r = get_rand_32();
                memcpy(s_pair_a1 + i, &r, 4);
            }
            d[0] = 0x00;
            reverse16(d + 1, s_pair_a1);
            init_submit(S2_CMD_PAIR, S2_SUB_PAIR_EXCHANGE_KEY, d, sizeof d);
            return;
        }
        case ST_PAIR_CONFIRM: {
            uint8_t a2[16], d[17];
            for (int i = 0; i < 16; i += 4) {
                uint32_t r = get_rand_32();
                memcpy(a2 + i, &r, 4);
            }
            d[0] = 0x00;
            reverse16(d + 1, a2);
            init_submit(S2_CMD_PAIR, S2_SUB_PAIR_CONFIRM_LTK, d, sizeof d);
            return;
        }
        case ST_PAIR_FINALIZE: {
            uint8_t d[1] = {0x00};
            init_submit(S2_CMD_PAIR, S2_SUB_PAIR_FINALIZE, d, sizeof d);
            return;
        }
        case ST_VIB_SAMPLE: {
            uint8_t d[4] = {0x03, 0, 0, 0};   // the "connected" click
            init_submit(S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d);
            return;
        }
        case ST_LEDS: {
            uint8_t d[8] = {s_led_wanted, 0, 0, 0, 0, 0, 0, 0};
            init_submit(S2_CMD_LEDS, S2_SUB_LEDS_SET_PATTERN, d, sizeof d);
            return;
        }
        case ST_FEATURE_MASK: {
            uint8_t d[4] = {FEATURE_FLAGS, 0, 0, 0};
            init_submit(S2_CMD_FEATURE, S2_SUB_FEATURE_SET_MASK, d, sizeof d);
            return;
        }
        case ST_READ_CAL_L:
            init_submit_read(S2_ADDR_FACTORY_STICK_L, 0x40);
            return;
        case ST_READ_CAL_R:
            init_submit_read(S2_ADDR_FACTORY_STICK_R, 0x40);
            return;
        case ST_READ_USER_L:
            init_submit_read(S2_ADDR_USER_STICK_L, 0x40);
            return;
        case ST_READ_USER_R:
            init_submit_read(S2_ADDR_USER_STICK_R, 0x40);
            return;
        case ST_READ_GC_TRIGGERS:
            if (!s_map.is_gamecube) {
                s_init_step++;
                continue;
            }
            init_submit_read(S2_ADDR_GC_TRIGGERS, 0x02);
            return;
        case ST_VIB_DATA: {
            // Sent by the console before enabling features (contents not understood).
            static const uint8_t d[20] = {0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                                          0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            init_submit(S2_CMD_VIBRATION, S2_SUB_VIB_SEND_DATA, d, sizeof d);
            return;
        }
        case ST_FEATURE_ENABLE: {
            uint8_t d[4] = {FEATURE_FLAGS, 0, 0, 0};
            init_submit(S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, d, sizeof d);
            return;
        }
        case ST_DONE:
        default:
            after_init();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void phase(gatt_phase_t p) {
    s_phase = p;
    s_phase_deadline = make_timeout_time_ms(GATT_PHASE_TIMEOUT_MS);
}

static void after_init(void) {
    if (s_rate_desc_handle) {
        static uint8_t rate[2] = {0x85, 0x00};
        phase(GP_RATE_DESC);
        if (gatt_client_write_characteristic_descriptor_using_descriptor_handle(gatt_handler, s_con, s_rate_desc_handle,
                                                                               sizeof rate, rate) == ERROR_CODE_SUCCESS) {
            return;
        }
    }
    phase(GP_INPUT_CCCD);
    gatt_client_listen_for_characteristic_value_updates(&s_notif_input, gatt_handler, s_con, &s_ch_input);
    if (gatt_client_write_client_characteristic_configuration(gatt_handler, s_con, &s_ch_input,
            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION) != ERROR_CODE_SUCCESS) {
        drop_connection("could not enable input notifications");
    }
}

static bool uuid_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

static void on_input_notification(const uint8_t *value, uint16_t len) {
    s2_input_t in;
    if (!s2_parse_input_report(value, len, &in)) return;
    s_input = in;
    s_input_seq++;
    s_info.reports++;
    s_rate_count++;
    s_info.battery_mv = in.battery_mv;
    s_info.charge_state = in.charge_state;

    // Gyro range auto detection: if the IMU timestamp advances in real
    // microseconds the gyro is +-2000 dps over int16; otherwise use the
    // alternative scale SDL found on such controllers.
    if (!s_imu_detected && in.imu_timestamp) {
        if (s_imu_samples == 0) {
            s_imu_first_ts = in.imu_timestamp;
            s_imu_first_time = get_absolute_time();
        }
        s_imu_samples++;
        int64_t elapsed = absolute_time_diff_us(s_imu_first_time, get_absolute_time());
        if (elapsed > GYRO_DETECT_MS * 1000) {
            float ratio = (float)(uint32_t)(in.imu_timestamp - s_imu_first_ts) / (float)elapsed;
            bool micro = ratio > 0.85f && ratio < 1.15f;
            s_imu_detected = true;
            s_info.gyro_range_detected = micro ? GYRO_RANGE_16_4 : GYRO_RANGE_14_3;
            LOG("s2: IMU timestamp ratio %.3f -> gyro %s LSB/dps", (double)ratio, micro ? "16.4" : "14.3");
        }
    }
    uint8_t range = g_settings.gyro_range;
    if (range == GYRO_RANGE_AUTO) range = s_imu_detected ? s_info.gyro_range_detected : GYRO_RANGE_16_4;
    s_map.gyro_lsb_per_dps = range == GYRO_RANGE_14_3 ? S2_GYRO_LSB_PER_DPS_B : S2_GYRO_LSB_PER_DPS_A;

    if (s_cal_active) {
        for (int i = 0; i < 3; i++) s_cal_sum[i] += in.gyro[i];
        s_cal_n++;
    }
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t ev = hci_event_packet_get_type(packet);

    if (ev == GATT_EVENT_NOTIFICATION) {
        uint16_t h = gatt_event_notification_get_value_handle(packet);
        const uint8_t *v = gatt_event_notification_get_value(packet);
        uint16_t len = gatt_event_notification_get_value_length(packet);
        if (s_have_input && h == s_ch_input.value_handle) on_input_notification(v, len);
        else if (s_have_cmd_rsp && h == s_ch_cmd_rsp.value_handle) cmd_on_response(v, len);
        return;
    }

    switch (ev) {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
        gatt_event_service_query_result_get_service(packet, &s_service);
        s_have_service = true;
        break;
    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        if (uuid_eq(c.uuid128, S2_UUID_INPUT_COMMON)) { s_ch_input = c; s_have_input = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_CMD_WRITE)) { s_ch_cmd = c; s_have_cmd = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_CMD_RESPONSE)) { s_ch_cmd_rsp = c; s_have_cmd_rsp = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_VIB_PRO) || uuid_eq(c.uuid128, S2_UUID_VIB_JOYCON_L) ||
                 uuid_eq(c.uuid128, S2_UUID_VIB_JOYCON_R)) { s_ch_vib = c; s_have_vib = true; }
        break;
    }
    case GATT_EVENT_ALL_CHARACTERISTIC_DESCRIPTORS_QUERY_RESULT: {
        gatt_client_characteristic_descriptor_t d;
        gatt_event_all_characteristic_descriptors_query_result_get_characteristic_descriptor(packet, &d);
        if (uuid_eq(d.uuid128, S2_UUID_REPORT_RATE_DESC)) s_rate_desc_handle = d.handle;
        break;
    }
    case GATT_EVENT_MTU:
        s_info.mtu = gatt_event_mtu_get_MTU(packet);
        break;
    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t status = gatt_event_query_complete_get_att_status(packet);
        switch (s_phase) {
        case GP_SERVICES:
            if (!s_have_service) {
                drop_connection("Switch 2 HID service not found");
                break;
            }
            phase(GP_CHARACTERISTICS);
            gatt_client_discover_characteristics_for_service(gatt_handler, s_con, &s_service);
            break;
        case GP_CHARACTERISTICS:
            if (!s_have_input || !s_have_cmd || !s_have_cmd_rsp) {
                drop_connection("required characteristics missing");
                break;
            }
            LOG("s2: handles input=%04x cmd=%04x rsp=%04x vib=%04x", s_ch_input.value_handle, s_ch_cmd.value_handle,
                s_ch_cmd_rsp.value_handle, s_have_vib ? s_ch_vib.value_handle : 0);
            phase(GP_DESCRIPTORS);
            gatt_client_discover_characteristic_descriptors(gatt_handler, s_con, &s_ch_input);
            break;
        case GP_DESCRIPTORS:
            phase(GP_CMD_CCCD);
            gatt_client_listen_for_characteristic_value_updates(&s_notif_cmd, gatt_handler, s_con, &s_ch_cmd_rsp);
            gatt_client_write_client_characteristic_configuration(gatt_handler, s_con, &s_ch_cmd_rsp,
                GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            break;
        case GP_CMD_CCCD:
            if (status != ATT_ERROR_SUCCESS) {
                drop_connection("could not enable command responses");
                break;
            }
            phase(GP_INIT);
            set_state(S2_LINK_INITIALISING);
            s_init_step = ST_UNK07;
            s_init_tries = 0;
            init_run();
            break;
        case GP_RATE_DESC:
            if (status != ATT_ERROR_SUCCESS) LOG("s2: report rate descriptor write failed (0x%02x)", status);
            s_rate_desc_handle = 0;
            after_init();
            break;
        case GP_INPUT_CCCD:
            if (status != ATT_ERROR_SUCCESS) {
                drop_connection("could not enable input notifications");
                break;
            }
            phase(GP_READY);
            gatt_client_get_mtu(s_con, &s_info.mtu);
            LOG("s2: controller ready (ATT MTU %u)", s_info.mtu);
            s_rate_window = make_timeout_time_ms(1000);
            s_rate_count = 0;
            set_state(S2_LINK_READY);
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// GAP / HCI
// ---------------------------------------------------------------------------
static bool adv_find_manufacturer(const uint8_t *data, uint8_t len, const uint8_t **out, uint8_t *out_len) {
    ad_context_t ctx;
    for (ad_iterator_init(&ctx, len, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
        if (ad_iterator_get_data_type(&ctx) == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
            *out = ad_iterator_get_data(&ctx);
            *out_len = ad_iterator_get_data_len(&ctx);
            return true;
        }
    }
    return false;
}

static void on_advertisement(const uint8_t *packet) {
    if (s_state != S2_LINK_SCANNING) return;
    const uint8_t *mdata;
    uint8_t mlen;
    if (!adv_find_manufacturer(gap_event_advertising_report_get_data(packet),
                               gap_event_advertising_report_get_data_length(packet), &mdata, &mlen)) {
        return;
    }
    s2_adv_info_t adv;
    if (!s2_parse_manufacturer_data(mdata, mlen, &adv)) return;

    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    bd_addr_type_t type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
    s_info.last_rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);

    if (adv.pid != S2_PID_PRO2 && adv.pid != S2_PID_GAMECUBE) {
        static uint16_t logged_pid;
        if (logged_pid != adv.pid) {
            LOG("s2: ignoring unsupported controller pid %04x at %s", adv.pid, bd_addr_to_str(addr));
            logged_pid = adv.pid;
        }
        return;
    }

    bd_addr_t local;
    gap_local_bd_addr(local);
    bool for_us = true;
    for (int i = 0; i < 6; i++) {
        if (adv.host_addr_le[i] != local[5 - i]) for_us = false;
    }
    bool is_bonded = g_settings.bonded && memcmp(addr, g_settings.ctrl_addr, 6) == 0;

    if (s_paused) {
        // While the USB host sleeps we only watch for the bonded controller
        // waking up (a button press) so the host can be woken. Right after we
        // dropped the link the controller keeps advertising on its own for a
        // while, so those adverts are ignored.
        if (for_us && time_reached(s_pause_quiet_until) && time_reached(s_seen_hook_next)) {
            s_seen_hook_next = make_timeout_time_ms(1000);
            s2_link_hook_controller_seen();
        }
        return;
    }

    bool connect = false;
    if (adv.pairing_mode) {
        LOG("s2: controller %s in pairing mode", bd_addr_to_str(addr));
        connect = true;
        s_need_pairing = true;
    } else if (for_us || is_bonded) {
        if (!for_us) {
            // Bonded here but it wants to reconnect to another host (e.g. a console).
            return;
        }
        connect = true;
        s_need_pairing = !is_bonded;
    }
    if (!connect) return;

    gap_stop_scan();
    memcpy(s_peer, addr, 6);
    s_peer_type = type;
    s_peer_pid = adv.pid;
    s_map.is_gamecube = adv.pid == S2_PID_GAMECUBE;
    gap_set_connection_parameters(0x0030, 0x0030, CONN_INTERVAL, CONN_INTERVAL, 0, CONN_SUPERVISION, 0, 0);
    LOG("s2: connecting to %s (type %d, pid %04x, pairing=%d)", bd_addr_to_str(addr), type, adv.pid,
        s_need_pairing);
    if (gap_connect(addr, type) != ERROR_CODE_SUCCESS) {
        start_scanning();
        return;
    }
    set_state(S2_LINK_CONNECTING);
    s_phase_deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
}

static void on_connected(hci_con_handle_t con, uint8_t status, uint16_t interval) {
    if (s_state != S2_LINK_CONNECTING) return;
    if (status != ERROR_CODE_SUCCESS) {
        LOG("s2: connection failed (0x%02x)", status);
        start_scanning();
        return;
    }
    reset_session();
    s_con = con;
    s_info.conn_interval = interval;
    memcpy(s_info.addr, s_peer, 6);
    s_info.pid = s_peer_pid;
    s_info.paired_this_session = false;
    s_map.is_gamecube = s_peer_pid == S2_PID_GAMECUBE;
    LOG("s2: connected, handle %04x, interval %u", con, interval);
    set_state(S2_LINK_DISCOVERING);
    phase(GP_SERVICES);
    gatt_client_discover_primary_services_by_uuid128(gatt_handler, s_con, S2_UUID_HID_SERVICE);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            LOG("s2: Bluetooth ready, local address %s", bd_addr_to_str(local));
            start_scanning();
        }
        break;
    case GAP_EVENT_ADVERTISING_REPORT:
        on_advertisement(packet);
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            on_connected(gap_subevent_le_connection_complete_get_connection_handle(packet),
                         gap_subevent_le_connection_complete_get_status(packet),
                         gap_subevent_le_connection_complete_get_conn_interval(packet));
        }
        break;
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
            s_info.conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            LOG("s2: connection interval now %u", s_info.conn_interval);
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == s_con) {
            LOG("s2: disconnected (reason 0x%02x)", hci_event_disconnection_complete_get_reason(packet));
            reset_session();
            amiibo_clear();
            s_led_sent = 0xFF;
            start_scanning();
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void s2_link_init(void) {
    memset(&s_info, 0, sizeof s_info);
    reset_session();
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(0);
    gatt_client_init();
    // Switch 2 controllers terminate the link if SMP pairing is attempted;
    // never ask for a security level that would trigger it.
    gatt_client_set_required_security_level(LEVEL_0);

    s_hci_cb.callback = hci_handler;
    hci_add_event_handler(&s_hci_cb);
    hci_power_control(HCI_POWER_ON);
}

void s2_link_task(void) {
    // Connection / discovery watchdogs.
    if (s_state == S2_LINK_CONNECTING && time_reached(s_phase_deadline)) {
        LOG("s2: connect timed out");
        gap_connect_cancel();
        start_scanning();
    }
    if ((s_state == S2_LINK_DISCOVERING || s_state == S2_LINK_INITIALISING) && s_phase != GP_INIT &&
        s_phase != GP_NONE && time_reached(s_phase_deadline)) {
        s_phase_deadline = make_timeout_time_ms(GATT_PHASE_TIMEOUT_MS);
        drop_connection("GATT timeout");
    }
    if (s_state == S2_LINK_OFF && hci_get_state() == HCI_STATE_WORKING) start_scanning();

    if (s_state == S2_LINK_READY && time_reached(s_rate_window)) {
        s_info.report_rate_hz = (float)s_rate_count;
        s_rate_count = 0;
        s_rate_window = make_timeout_time_ms(1000);
    }

    if (s_cal_active && time_reached(s_cal_end)) {
        s_cal_active = false;
        if (s_cal_n > 50) {
            for (int i = 0; i < 3; i++) g_settings.gyro_bias[i] = (int16_t)(s_cal_sum[i] / s_cal_n);
            settings_save_later();
            LOG("s2: gyro bias %d %d %d (%d samples)", g_settings.gyro_bias[0], g_settings.gyro_bias[1],
                g_settings.gyro_bias[2], s_cal_n);
        } else {
            LOG("s2: gyro calibration failed (no IMU data)");
        }
    }

    cmd_pump();
    if (s_state == S2_LINK_READY) {
        led_task();
        nfc_task();
        rumble_task();
    }
}

s2_link_state_t s2_link_state(void) {
    return s_state;
}

void s2_link_get_info(s2_link_info_t *out) {
    *out = s_info;
    out->state = s_state;
    out->gyro_cal_busy = s_cal_active;
    if (!s_imu_detected) out->gyro_range_detected = 0;
}

bool s2_link_get_input(s2_input_t *out, uint32_t *seq) {
    if (s_state != S2_LINK_READY) return false;
    *out = s_input;
    if (seq) *seq = s_input_seq;
    return true;
}

const mapping_ctx_t *s2_link_mapping_ctx(void) {
    return &s_map;
}

void s2_link_disconnect(void) {
    if (s_con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con);
}

void s2_link_forget(void) {
    g_settings.bonded = 0;
    memset(g_settings.ctrl_addr, 0, sizeof g_settings.ctrl_addr);
    settings_save_later();
    s2_link_disconnect();
    LOG("s2: pairing forgotten");
}

void s2_link_set_paused(bool paused) {
    if (paused == s_paused) return;
    s_paused = paused;
    LOG("s2: %s", paused ? "paused" : "resumed");
    if (paused) {
        s_pause_quiet_until = make_timeout_time_ms(20000);
        if (s_state == S2_LINK_CONNECTING) {
            gap_connect_cancel();
            start_scanning();
        } else if (s_con != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(s_con);
        }
    }
}

void s2_link_start_gyro_calibration(void) {
    if (s_state != S2_LINK_READY) return;
    s_cal_sum[0] = s_cal_sum[1] = s_cal_sum[2] = 0;
    s_cal_n = 0;
    s_cal_active = true;
    s_cal_end = make_timeout_time_ms(2000);
    LOG("s2: gyro calibration started, keep the controller still");
}
