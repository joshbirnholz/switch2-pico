#include "s2_link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "s2_transport.h"
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
#define GYRO_DETECT_MS          1500


// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef enum {
    PH_NONE,
    PH_DISCOVERY,        // transport is discovering GATT
    PH_INIT,             // command sequence running
    PH_INPUT_ENABLE,     // waiting for input notifications to be enabled
    PH_READY,
} link_phase_t;

static s2_link_state_t s_state = S2_LINK_OFF;
static link_phase_t s_phase = PH_NONE;
static bool s_connected;           // a link to the controller exists
static uint8_t s_peer[6];
static uint8_t s_peer_type;
static uint16_t s_peer_pid;
static bool s_paused;              // host asleep: scan for wake-ups but don't connect
static uint32_t s_pause_quiet_until;
static uint32_t s_sleep_quiet_until;   // after s2_link_let_controller_sleep()
static bool s_pair_open;               // pairing window (settings.pair_button)
static uint32_t s_pair_until;
static uint32_t s_pair_ignored_log;
static uint32_t s_unbonded_log;
static bool s_sleep_quiet;
static uint32_t s_seen_hook_next;
static uint32_t s_phase_deadline;

static s2_input_t s_input;
static uint32_t s_input_seq;
static mapping_ctx_t s_map;
static s2_link_info_t s_info;
static uint32_t s_rate_count;
static uint32_t s_rate_window;

static bool s_need_pairing;
static uint8_t s_pair_a1[16];

// Gyro range detection (see mapping.h)
static uint32_t s_imu_first_ts;
static uint32_t s_imu_first_time;
static int s_imu_samples;
static bool s_imu_detected;

// Gyro bias calibration
static bool s_cal_active;
static int32_t s_cal_sum[3];
static int s_cal_n;
static uint32_t s_cal_end;

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
static uint32_t s_cmd_deadline;

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
    if (!s_connected || s_cmd_count == 0) return;
    cmd_t *c = &s_cmdq[s_cmd_head];
    if (s_cmd_in_flight) {
        if (!platform_time_reached(s_cmd_deadline)) return;
        if (c->tries <= CMD_RETRIES) {
            LOG("s2: command 0x%02x/0x%02x timed out, retrying", c->cmd, c->sub);
            s_cmd_in_flight = false;
        } else {
            LOG("s2: command 0x%02x/0x%02x failed", c->cmd, c->sub);
            cmd_finish(false, NULL);
            return;
        }
    }
    s2t_write_result_t st = s2t_write(S2T_CHAR_COMMAND, c->buf, c->len);
    if (st == S2T_WRITE_ERROR) {
        LOG("s2: command 0x%02x/0x%02x (%u bytes) could not be sent (ATT MTU %u)", c->cmd, c->sub, c->len, s2t_mtu());
        cmd_finish(false, NULL);
        return;
    }
    if (st != S2T_WRITE_OK) return;   // TX buffers full: try again next pass
    c->tries++;
    s_cmd_in_flight = true;
    s_cmd_deadline = platform_deadline_ms(CMD_TIMEOUT_MS);
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
    return platform_millis();
}

static const char *addr_str(const uint8_t a[6]) {
    static char buf[18];
    snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return buf;
}

static void reverse16(uint8_t *dst, const uint8_t *src) {
    for (int i = 0; i < 16; i++) dst[i] = src[15 - i];
}

static void set_state(s2_link_state_t st) {
    if (st == s_state) return;
    bool was_ready = s_state == S2_LINK_READY;
    s_state = st;
    LOG("s2: state -> %s", s2_link_state_name(st));
    if (st == S2_LINK_READY && s_pair_open) {
        s_pair_open = false;
        LOG("s2: pairing window closed (controller connected)");
    }
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

static bool s_low_duty_scan;
static uint8_t s_fail_stage;          // see s2_link_last_failure()
static uint8_t s_fail_reason;
static uint32_t s_fail_time;

static void record_failure(uint8_t stage, uint8_t reason) {
    s_fail_stage = stage;
    s_fail_reason = reason;
    s_fail_time = platform_millis();
    LOG("s2: connection attempt failed at stage %u (reason 0x%02x)", stage, reason);
}

static void start_scanning(void) {
    s2t_start_scan(s_low_duty_scan);
    set_state(S2_LINK_SCANNING);
}

static void reset_session(void) {
    s_connected = false;
    s_phase = PH_NONE;
    cmd_queue_reset();
    memset(&s_input, 0, sizeof s_input);
    s_imu_samples = 0;
    s_imu_detected = false;
    s_cal_active = false;
    s2_default_stick_cal(&s_map.cal_l);
    s2_default_stick_cal(&s_map.cal_r);
    s_map.is_gamecube = false;
    s_map.gc_trigger_neutral[0] = s_map.gc_trigger_neutral[1] = 30;
    s_map.gyro_lsb_per_dps = S2_GYRO_LSB_PER_DPS_A;
}

static void drop_connection(const char *why) {
    LOG("s2: dropping connection: %s", why);
    if (s_connected) s2t_disconnect();
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

// NSO GameCube controller: one classic motor, driven through its own
// vibration characteristic with 42-byte packets: 00, 0x50|seq, state (1 on,
// 0 off, 2 stop), zeros. Strength is the share of 12 ms slots it is on
// (error diffusion, as SDL does over USB).
#define GC_PACKET_LEN  42
#define GC_SLOT_MS     12
static float s_gc_err;
static bool s_gc_running;
static uint32_t s_gc_last_send;

static bool gc_motor_write(uint8_t state) {
    uint8_t pkt[GC_PACKET_LEN];
    memset(pkt, 0, sizeof pkt);
    pkt[1] = (uint8_t)(0x50 | (s_rum_seq & 0x0F));
    pkt[2] = state;
    if (s2t_write(S2T_CHAR_VIBRATION, pkt, sizeof pkt) != S2T_WRITE_OK) return false;
    s_rum_seq++;
    s_info.rumble_packets++;
    return true;
}

// duty 0..1; call often, it paces itself.
static void gc_motor_task(uint32_t now, float duty) {
    if (now - s_gc_last_send < GC_SLOT_MS) return;
    if (duty < 0.02f) {
        if (s_gc_running && gc_motor_write(2)) {   // stop
            s_gc_running = false;
            s_gc_err = 0.0f;
            s_gc_last_send = now;
        }
        return;
    }
    if (duty > 1.0f) duty = 1.0f;
    float err = s_gc_err + duty;
    uint8_t state = err >= 1.0f ? 1 : 0;
    if (state) err -= 1.0f;
    if (gc_motor_write(state)) {
        s_gc_err = err;
        s_gc_running = true;
        s_gc_last_send = now;
    }
}

static float host_rumble_magnitude(uint32_t now) {
    float mag = 0.0f;
    for (int i = 0; i < s_rum_nl; i++) mag = fmaxf(mag, fmaxf(s_rum_l[i].hi_amp, s_rum_l[i].lo_amp));
    for (int i = 0; i < s_rum_nr; i++) mag = fmaxf(mag, fmaxf(s_rum_r[i].hi_amp, s_rum_r[i].lo_amp));
    mag *= (float)settings_tuning(&g_settings, mapping_ctrl_type(&s_map)).rumble_strength / 100.0f;
    if (now - s_rum_last_host_ms > RUMBLE_IDLE_STOP_MS) mag = 0.0f;
    return mag;
}

// GameCube controller without the vibration characteristic: built-in samples.
static void gc_rumble_task(uint32_t now) {
    float mag = 0.0f;
    for (int i = 0; i < s_rum_nl; i++) mag = fmaxf(mag, fmaxf(s_rum_l[i].hi_amp, s_rum_l[i].lo_amp));
    for (int i = 0; i < s_rum_nr; i++) mag = fmaxf(mag, fmaxf(s_rum_r[i].hi_amp, s_rum_r[i].lo_amp));
    mag *= (float)settings_tuning(&g_settings, mapping_ctrl_type(&s_map)).rumble_strength / 100.0f;
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

// ---------------------------------------------------------------------------
// Feedback effects: a few timed steps, each a steady (high band, low band)
// tone on both actuators; played instead of the host's rumble while running.
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t ms;
    float hi_hz, hi_amp, lo_hz, lo_amp;
} haptic_step_t;

static const haptic_step_t HAP_TICK[] = {
    {22, 320.0f, 0.80f, 200.0f, 0.45f},
};
static const haptic_step_t HAP_THUMP[] = {
    {75, 260.0f, 0.45f, 130.0f, 1.00f},
};
// Built-in vibration samples (s2_link_play_sample): 1 buzz, 2 buzz + two
// beeps, 3 the "ba-thump" the controller plays when it connects, 4 the
// pairing "ka-chink", 5 a heavier 3, 6 / 7 the two halves of 4.
#define SAMPLE_BA_THUMP 3

static const struct {
    const haptic_step_t *steps;   // NULL: play built-in `sample`
    uint8_t n;
    uint8_t sample;
    const char *name;
} HAPTICS[S2_HAPTIC_COUNT] = {
    [S2_HAPTIC_TICK] = {HAP_TICK, 1, 0, "tick"},
    [S2_HAPTIC_BA_THUMP] = {NULL, 0, SAMPLE_BA_THUMP, "ba_thump"},
    [S2_HAPTIC_THUMP] = {HAP_THUMP, 1, 0, "thump"},
};

#define HAPTIC_FRAME_MS  5    // each of the 3 frames in a rumble packet
#define HAPTIC_PACKET_MS 15

static int s_hap = -1;        // effect playing, -1: none
static uint32_t s_hap_start;
static uint32_t s_hap_last_send;

const char *s2_link_haptic_name(s2_haptic_t effect) {
    return effect < S2_HAPTIC_COUNT ? HAPTICS[effect].name : "?";
}

void s2_link_haptic(s2_haptic_t effect) {
    if (s_state != S2_LINK_READY || effect >= S2_HAPTIC_COUNT) return;
    if (!HAPTICS[effect].steps) {
        s2_link_play_sample(HAPTICS[effect].sample);
        return;
    }
    if (!s2t_has_char(S2T_CHAR_VIBRATION)) {
        s2_link_test_rumble();
        return;
    }
    s_hap = effect;
    s_hap_start = now_ms();
    s_hap_last_send = s_hap_start - HAPTIC_PACKET_MS;
}

// Sample of the playing effect `t` ms after its start; false once it is over.
static bool haptic_at(uint32_t t, rumble_sample_t *out) {
    memset(out, 0, sizeof *out);
    out->hi_freq_hz = 320.0f;
    out->lo_freq_hz = 160.0f;
    const haptic_step_t *st = HAPTICS[s_hap].steps;
    for (int i = 0; i < HAPTICS[s_hap].n; i++) {
        if (t < st[i].ms) {
            if (st[i].hi_amp > 0 || st[i].lo_amp > 0) {
                *out = (rumble_sample_t){st[i].hi_hz, st[i].hi_amp, st[i].lo_hz, st[i].lo_amp};
            }
            return true;
        }
        t -= st[i].ms;
    }
    return false;
}

// Returns true while an effect owns the actuators.
static bool haptic_task(uint32_t now) {
    if (s_hap < 0) return false;
    if (s_map.is_gamecube) {
        // One motor: on while the effect's step is strong enough, then stop.
        rumble_sample_t smp;
        if (!haptic_at(now - s_hap_start, &smp)) {
            gc_motor_task(now, 0.0f);
            if (!s_gc_running) s_hap = -1;
            return true;
        }
        gc_motor_task(now, fmaxf(smp.hi_amp, smp.lo_amp) > 0.3f ? 1.0f : 0.0f);
        return true;
    }
    if (now - s_hap_last_send < HAPTIC_PACKET_MS) return true;
    uint32_t t = now - s_hap_start;
    rumble_sample_t smp[3];
    bool more = false;
    for (int k = 0; k < 3; k++) more |= haptic_at(t + (uint32_t)k * HAPTIC_FRAME_MS, &smp[k]);
    // Fixed strength and real frequencies: the same feel whatever the rumble settings.
    s2_rumble_params_t p = {.translate_freq = true, .freq_slope = g_settings.rumble_freq_slope, .strength_pct = 100};
    uint8_t pkt[S2_RUMBLE_PRO_PACKET_LEN];
    pkt[0] = 0x00;
    s2_rumble_encode_block(s_rum_seq, smp, 3, &p, pkt + 1);
    s2_rumble_encode_block(s_rum_seq, smp, 3, &p, pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    uint16_t len = s_peer_pid == S2_PID_PRO2 ? S2_RUMBLE_PRO_PACKET_LEN : 1 + S2_RUMBLE_BLOCK_LEN;
    if (s2t_write(S2T_CHAR_VIBRATION, pkt, len) != S2T_WRITE_OK) return true;
    s_rum_seq++;
    s_hap_last_send = now;
    s_info.rumble_packets++;
    if (!more) {
        // That was the silent tail; hand the actuators back to the host's rumble.
        s_hap = -1;
        s_rum_active = false;
        s_rum_dirty = true;
        s_rum_last_send_ms = now;
    }
    return true;
}

static rumble_sample_t s_test_l, s_test_r;
static uint32_t s_test_until;
static bool s_test_on;

void s2_link_test_motors(uint8_t left_strong, uint8_t right_weak, uint16_t ms) {
    rumble_from_motors(left_strong, right_weak, &s_test_l, &s_test_r);
    s_test_until = now_ms() + ms;
    s_test_on = true;
}

static void rumble_task(void) {
    if (s_test_on) {
        // Fed like host rumble; it fades out by itself once this stops.
        if ((int32_t)(now_ms() - s_test_until) >= 0) s_test_on = false;
        else s2_link_rumble_submit(&s_test_l, 1, &s_test_r, 1);
    }
    if (s_state != S2_LINK_READY) {
        s_hap = -1;
        s_gc_running = false;
        s_gc_err = 0.0f;
        return;
    }
    uint32_t now = now_ms();
    if (haptic_task(now) || !settings_tuning(&g_settings, mapping_ctrl_type(&s_map)).rumble_enabled) return;
    if (s_map.is_gamecube) {
        if (s2t_has_char(S2T_CHAR_VIBRATION)) gc_motor_task(now, host_rumble_magnitude(now));
        else gc_rumble_task(now);
        return;
    }
    if (!s2t_has_char(S2T_CHAR_VIBRATION)) return;

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
    if (s2t_write(S2T_CHAR_VIBRATION, pkt, len) == S2T_WRITE_OK) {
        s_rum_seq++;
        s_rum_dirty = false;
        s_rum_active = want_active;
        s_rum_last_send_ms = now;
        s_info.rumble_packets++;
    }
}

void s2_link_play_sample(uint8_t sample) {
    if (s_state != S2_LINK_READY) return;
    uint8_t d[4] = {sample, 0, 0, 0};
    cmd_submit(S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0);
}

void s2_link_test_rumble(void) {
    s2_link_play_sample(0x01);   // built-in low frequency buzz
}


// ---------------------------------------------------------------------------
// Player LEDs
// ---------------------------------------------------------------------------
static uint8_t s_led_wanted = 0x01;
static uint8_t s_led_sent = 0xFF;
static int s_led_override = -1;

void s2_link_set_player_leds(uint8_t pattern) {
    s_led_wanted = pattern & 0x0F;
}

void s2_link_set_led_override(int pattern) {
    s_led_override = pattern < 0 ? -1 : (pattern & 0x0F);
}

static void led_task(void) {
    uint8_t want = s_led_override >= 0 ? (uint8_t)s_led_override : s_led_wanted;
    if (s_state != S2_LINK_READY || want == s_led_sent) return;
    uint8_t d[8] = {want, 0, 0, 0, 0, 0, 0, 0};
    if (cmd_submit(S2_CMD_LEDS, S2_SUB_LEDS_SET_PATTERN, d, sizeof d, NULL, 0)) s_led_sent = want;
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
    if ((int)step != s_init_step || s_phase != PH_INIT) return;
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
        g_settings.ctrl_addr_type = s_peer_type;
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
            uint8_t local[6];
            s2t_local_address(local);
            uint8_t d[14];
            d[0] = 0x00;
            d[1] = 0x02;
            for (int i = 0; i < 6; i++) d[2 + i] = local[5 - i];
            memcpy(d + 8, d + 2, 6);
            LOG("s2: pairing with host address %s", addr_str(local));
            init_submit(S2_CMD_PAIR, S2_SUB_PAIR_SET_ADDRESS, d, sizeof d);
            return;
        }
        case ST_PAIR_KEY: {
            uint8_t d[17];
            for (int i = 0; i < 16; i += 4) {
                uint32_t r = platform_random32();
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
                uint32_t r = platform_random32();
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
// Link phases driven by the transport
// ---------------------------------------------------------------------------
static void phase(link_phase_t p) {
    s_phase = p;
    s_phase_deadline = platform_deadline_ms(GATT_PHASE_TIMEOUT_MS);
}

static void after_init(void) {
    // (The console also writes 0x85 0x00 to an undocumented "report rate"
    // descriptor; the working Linux implementation doesn't, so neither do we.)
    phase(PH_INPUT_ENABLE);
    s2t_enable_input();
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
            s_imu_first_time = platform_millis();
        }
        s_imu_samples++;
        uint32_t elapsed_ms = platform_millis() - s_imu_first_time;
        if (elapsed_ms > GYRO_DETECT_MS) {
            float ratio = (float)(uint32_t)(in.imu_timestamp - s_imu_first_ts) / ((float)elapsed_ms * 1000.0f);
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

// ---------------------------------------------------------------------------
// Transport callbacks (see s2_transport.h)
// ---------------------------------------------------------------------------
static bool adv_find_manufacturer(const uint8_t *data, uint16_t len, const uint8_t **out, uint8_t *out_len) {
    uint16_t i = 0;
    while (i + 1 < len) {
        uint8_t field_len = data[i];
        if (field_len == 0 || i + 1 + field_len > len) break;
        if (data[i + 1] == 0xFF) {   // manufacturer specific data
            *out = data + i + 2;
            *out_len = (uint8_t)(field_len - 1);
            return true;
        }
        i = (uint16_t)(i + 1 + field_len);
    }
    return false;
}

void s2c_on_stack_ready(void) {
    uint8_t local[6];
    s2t_local_address(local);
    LOG("s2: Bluetooth ready, local address %s", addr_str(local));
    start_scanning();
}

void s2c_on_advertisement(const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                          const uint8_t *adv_data, uint16_t adv_len) {
    if (s_state != S2_LINK_SCANNING) return;
    const uint8_t *mdata;
    uint8_t mlen;
    if (!adv_find_manufacturer(adv_data, adv_len, &mdata, &mlen)) return;
    s2_adv_info_t adv;
    if (!s2_parse_manufacturer_data(mdata, mlen, &adv)) return;
    s_info.last_rssi = rssi;

    if (adv.pid != S2_PID_PRO2 && adv.pid != S2_PID_GAMECUBE) {
        static uint16_t logged_pid;
        if (logged_pid != adv.pid) {
            LOG("s2: ignoring unsupported controller pid %04x at %s", adv.pid, addr_str(addr));
            logged_pid = adv.pid;
        }
        return;
    }

    uint8_t local[6];
    s2t_local_address(local);
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
        if (for_us && platform_time_reached(s_pause_quiet_until) && platform_time_reached(s_seen_hook_next)) {
            s_seen_hook_next = platform_deadline_ms(1000);
            s2_link_hook_controller_seen();
        }
        return;
    }

    // Pairing only on request (Sync button / page): pairing-mode adverts are
    // ignored outside the window, and inside it nothing else is accepted.
    if (s_pair_open && platform_time_reached(s_pair_until)) {
        s_pair_open = false;
        LOG("s2: pairing window closed (timed out)");
    }
    if (g_settings.pair_button) {
        if (s_pair_open && !adv.pairing_mode) return;
        if (!s_pair_open && adv.pairing_mode) {
            if (platform_time_reached(s_pair_ignored_log)) {
                s_pair_ignored_log = platform_deadline_ms(10000);
                LOG("s2: controller %s in pairing mode ignored: press Sync on the dongle to pair", addr_str(addr));
            }
            return;
        }
    }

    // Right after we let the controller sleep it keeps advertising on its own
    // for a while; reconnect only once it advertises again later (a button
    // press). Pairing mode always counts.
    if (s_sleep_quiet && !adv.pairing_mode) {
        if (!platform_time_reached(s_sleep_quiet_until)) return;
        s_sleep_quiet = false;
    }

    if (adv.pairing_mode) {
        LOG("s2: controller %s in pairing mode", addr_str(addr));
        s_need_pairing = true;
    } else if (for_us) {
        // A controller that still remembers us but that we don't (pairing
        // forgotten, or another controller paired since) must be paired
        // again with Sync, like a new one.
        if (!is_bonded) {
            if (platform_time_reached(s_unbonded_log)) {
                s_unbonded_log = platform_deadline_ms(10000);
                LOG("s2: controller %s isn't paired with the dongle: hold Sync on it to pair", addr_str(addr));
            }
            return;
        }
        s_need_pairing = false;
    } else {
        return;   // reconnecting to another host (e.g. a console)
    }

    s2t_stop_scan();
    memcpy(s_peer, addr, 6);
    s_peer_type = addr_type;
    s_peer_pid = adv.pid;
    s_map.is_gamecube = adv.pid == S2_PID_GAMECUBE;
    LOG("s2: connecting to %s (type %u, pid %04x, pairing=%d)", addr_str(addr), addr_type, adv.pid, s_need_pairing);
    if (!s2t_connect(addr, addr_type)) {
        record_failure(1, 0xFE);
        start_scanning();
        return;
    }
    set_state(S2_LINK_CONNECTING);
    s_phase_deadline = platform_deadline_ms(CONNECT_TIMEOUT_MS);
}

void s2c_on_link_up(uint16_t conn_interval) {
    if (s_state != S2_LINK_CONNECTING) return;
    reset_session();
    s_connected = true;
    s_info.conn_interval = conn_interval;
    memcpy(s_info.addr, s_peer, 6);
    s_info.pid = s_peer_pid;
    s_info.paired_this_session = false;
    s_map.is_gamecube = s_peer_pid == S2_PID_GAMECUBE;
    LOG("s2: connected, interval %u", conn_interval);
    set_state(S2_LINK_DISCOVERING);
    phase(PH_DISCOVERY);
}

void s2c_on_connect_failed(uint8_t reason) {
    if (s_state != S2_LINK_CONNECTING) return;
    LOG("s2: connection failed (0x%02x)", reason);
    record_failure(1, reason);
    start_scanning();
}

void s2c_on_gatt_ready(bool ok, const char *error) {
    if (s_phase != PH_DISCOVERY) return;
    if (!ok) {
        drop_connection(error ? error : "GATT discovery failed");
        return;
    }
    phase(PH_INIT);
    set_state(S2_LINK_INITIALISING);
    s_init_step = ST_UNK07;
    s_init_tries = 0;
    init_run();
}

void s2c_on_command_response(const uint8_t *data, uint16_t len) {
    cmd_on_response(data, len);
}

void s2c_on_input_report(const uint8_t *data, uint16_t len) {
    if (s_phase == PH_READY || s_phase == PH_INPUT_ENABLE) on_input_notification(data, len);
}

void s2c_on_input_enabled(bool ok) {
    if (s_phase != PH_INPUT_ENABLE) return;
    if (!ok) {
        drop_connection("could not enable input notifications");
        return;
    }
    phase(PH_READY);
    s_info.mtu = s2t_mtu();
    LOG("s2: controller ready (ATT MTU %u)", s_info.mtu);
    s_rate_window = platform_deadline_ms(1000);
    s_rate_count = 0;
    set_state(S2_LINK_READY);
}

void s2c_on_conn_interval(uint16_t conn_interval) {
    s_info.conn_interval = conn_interval;
    LOG("s2: connection interval now %u", conn_interval);
}

void s2c_on_disconnected(uint8_t reason) {
    if (!s_connected && s_state != S2_LINK_CONNECTING) return;
    LOG("s2: disconnected (reason 0x%02x)", reason);
    if (s_state == S2_LINK_CONNECTING) record_failure(1, reason);
    else if (s_state == S2_LINK_DISCOVERING) record_failure(2, reason);
    else if (s_state == S2_LINK_INITIALISING) record_failure(s_init_step <= ST_PAIR_FINALIZE ? 3 : 4, reason);
    reset_session();
    s_led_sent = 0xFF;
    start_scanning();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void s2_link_set_low_duty_scan(bool low) {
    if (low == s_low_duty_scan) return;
    s_low_duty_scan = low;
    if (s_state == S2_LINK_SCANNING) start_scanning();
}

bool s2_link_last_failure(uint8_t *stage, uint8_t *reason, uint32_t *age_ms) {
    if (!s_fail_stage) return false;
    *stage = s_fail_stage;
    *reason = s_fail_reason;
    *age_ms = platform_millis() - s_fail_time;
    return true;
}

void s2_link_init(void) {
    memset(&s_info, 0, sizeof s_info);
    reset_session();
    s2t_init();
}

void s2_link_task(void) {
    s2t_task();

    // Connection / discovery watchdogs.
    if (s_state == S2_LINK_CONNECTING && platform_time_reached(s_phase_deadline)) {
        LOG("s2: connect timed out");
        record_failure(1, 0xFF);
        s2t_cancel_connect();
        start_scanning();
    }
    if (s_connected && (s_phase == PH_DISCOVERY || s_phase == PH_INPUT_ENABLE) &&
        platform_time_reached(s_phase_deadline)) {
        s_phase_deadline = platform_deadline_ms(GATT_PHASE_TIMEOUT_MS);
        drop_connection(s_phase == PH_DISCOVERY ? "GATT discovery timeout" : "input enable timeout");
    }
    if (s_state == S2_LINK_OFF && s2t_ready()) start_scanning();

    if (s_state == S2_LINK_READY && platform_time_reached(s_rate_window)) {
        s_info.report_rate_hz = (float)s_rate_count;
        s_rate_count = 0;
        s_rate_window = platform_deadline_ms(1000);
    }

    if (s_cal_active && platform_time_reached(s_cal_end)) {
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
    if (s_connected) s2t_disconnect();
}

void s2_link_start_pairing(uint32_t ms) {
    s_pair_open = true;
    s_pair_until = platform_deadline_ms(ms);
    s_sleep_quiet = false;
    LOG("s2: pairing window open for %lu s: hold Sync on the controller", (unsigned long)(ms / 1000));
    // A connected controller would keep the radio busy: let the new one in.
    if (s_state == S2_LINK_CONNECTING) {
        s2t_cancel_connect();
        start_scanning();
    } else if (s_connected) {
        s2t_disconnect();
    }
}

void s2_link_stop_pairing(void) {
    if (!s_pair_open) return;
    s_pair_open = false;
    LOG("s2: pairing window closed");
}

bool s2_link_pairing_open(void) {
    if (s_pair_open && platform_time_reached(s_pair_until)) {
        s_pair_open = false;
        LOG("s2: pairing window closed (timed out)");
    }
    return s_pair_open;
}

uint32_t s2_link_pairing_left_ms(void) {
    if (!s2_link_pairing_open()) return 0;
    int32_t left = (int32_t)(s_pair_until - platform_millis());
    return left > 0 ? (uint32_t)left : 0;
}

void s2_link_let_controller_sleep(void) {
    if (!s_connected) return;
    s_sleep_quiet = true;
    s_sleep_quiet_until = platform_deadline_ms(20000);
    s2t_disconnect();
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
        s_pause_quiet_until = platform_deadline_ms(20000);
        if (s_state == S2_LINK_CONNECTING) {
            s2t_cancel_connect();
            start_scanning();
        } else if (s_connected) {
            s2t_disconnect();
        }
    }
}

void s2_link_start_gyro_calibration(void) {
    if (s_state != S2_LINK_READY) return;
    s_cal_sum[0] = s_cal_sum[1] = s_cal_sum[2] = 0;
    s_cal_n = 0;
    s_cal_active = true;
    s_cal_end = platform_deadline_ms(2000);
    LOG("s2: gyro calibration started, keep the controller still");
}
