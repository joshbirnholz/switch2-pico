#include "s2_link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "s2_transport.h"
#include "app.h"
#include "battery.h"
#include "joycon.h"
#include "settings.h"

// One controller at a time, or a Joy-Con 2 (L) and (R) together: up to
// S2T_LINKS links, each with its own connection, command queue,
// initialisation, rumble and battery polling. The rest of the firmware sees
// one controller (s2_link_get_input() merges a Joy-Con pair, see joycon.h).

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
// A Joy-Con 2 used as a pair whose partner hasn't connected this long counts
// as a single (sideways) one.
#define HALF_PAIR_MS            30000

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

struct link;
typedef void (*cmd_cb_t)(struct link *k, bool ok, const s2_response_t *rsp, uint32_t ctx);

typedef struct {
    uint8_t buf[96];
    uint8_t len;
    uint8_t cmd, sub;
    cmd_cb_t cb;
    uint32_t ctx;
    uint8_t tries;
} cmd_t;

#define CMD_QUEUE_LEN 12

typedef struct link {
    uint8_t idx;
    s2_link_state_t state;   // S2_LINK_OFF: free
    link_phase_t phase;
    bool connected;          // a link to the controller exists
    uint8_t peer[6];
    uint8_t peer_type;
    uint16_t pid;
    bool need_pairing;
    uint8_t pair_a1[16];
    uint32_t phase_deadline;

    s2_input_t input;
    uint32_t input_seq;
    s2_stick_cal_t cal_l, cal_r;
    bool cal_l_read, cal_r_read;   // the controller had a calibration there
    uint8_t gc_trigger_neutral[2];
    float gyro_lsb_per_dps;
    s2_link_info_t info;
    uint32_t rate_count, rate_window;

    // Gyro range detection (see mapping.h)
    uint32_t imu_first_ts, imu_first_time;
    int imu_samples;
    bool imu_detected;

    cmd_t cmdq[CMD_QUEUE_LEN];
    int cmd_head, cmd_count;
    bool cmd_in_flight;
    uint32_t cmd_deadline;

    int init_step;
    uint8_t init_tries;
    uint8_t led_sent;
    uint8_t features;        // feature flags enabled on the controller
    bool features_busy;

    // Rumble
    bool rum_dirty, rum_active;
    uint32_t rum_last_send_ms;
    uint8_t rum_seq;
    float gc_last_mag;
    uint32_t gc_last_trigger_ms;
    float gc_err;
    bool gc_running;
    uint32_t gc_last_send;
    uint32_t hap_last_send;
    bool hap_done;

    // Controller's own battery level
    bool power_off, power_busy, power_check;
    uint8_t power_fails;
    uint32_t power_next, power_check_at, power_reports, power_busy_until;

    // Joy-Con 2 optical sensor
    joycon_mouse_track_t mouse;
    int32_t mouse_dx, mouse_dy;
} link_t;

static link_t s_links[S2T_LINKS];
static int s_connecting = -1;      // link with a connection attempt running
static bool s_scanning;
static bool s_low_duty_scan;
static ctrl_type_t s_type = CTRL_PRO;   // whose profiles apply
static ctrl_type_t s_type_before;       // ... before a second Joy-Con 2 started connecting
static bool s_pair_complete;            // both Joy-Con 2 were ready this session
static uint32_t s_half_since;           // a pair has run with one side since then
static bool s_any_ready;

static bool s_paused;              // host asleep: scan for wake-ups but don't connect
static uint32_t s_pause_quiet_until;
static uint32_t s_sleep_quiet_until;   // after s2_link_let_controller_sleep()
static bool s_pair_open;               // pairing window (settings.pair_button)
static uint32_t s_pair_until;
static uint32_t s_pair_ignored_log;
static uint32_t s_unbonded_log;
static bool s_sleep_quiet;
static uint32_t s_seen_hook_next;
static int8_t s_last_rssi;
static s2_link_info_t s_last_info;     // the last controller's, once none is connected

// The controller as the rest of the firmware sees it.
static s2_input_t s_merged;
static uint32_t s_merged_seq;
static mapping_ctx_t s_map;

// Gyro bias calibration (on the link that provides motion)
static bool s_cal_active;
static int32_t s_cal_sum[3];
static int s_cal_n;
static uint32_t s_cal_end;

static uint8_t s_fail_stage;          // see s2_link_last_failure()
static uint8_t s_fail_reason;
static uint32_t s_fail_time;

static void power_start(link_t *k);
static void refresh_merged(void);

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

static bool pid_is_joycon(uint16_t pid) {
    return pid == S2_PID_JOYCON2_L || pid == S2_PID_JOYCON2_R;
}

static ctrl_type_t type_of_pid(uint16_t pid) {
    switch (pid) {
    case S2_PID_GAMECUBE: return CTRL_GAMECUBE;
    case S2_PID_JOYCON2_L: return CTRL_JOYCON_L;
    case S2_PID_JOYCON2_R: return CTRL_JOYCON_R;
    default: return CTRL_PRO;
    }
}

static const char *pid_name(uint16_t pid) {
    switch (pid) {
    case S2_PID_PRO2: return "Pro Controller";
    case S2_PID_GAMECUBE: return "GameCube Controller";
    case S2_PID_JOYCON2_L: return "Joy-Con 2 (L)";
    case S2_PID_JOYCON2_R: return "Joy-Con 2 (R)";
    default: return "controller";
    }
}

static bool link_used(const link_t *k) {
    return k->state != S2_LINK_OFF;
}

static int links_used(void) {
    int n = 0;
    for (int i = 0; i < S2T_LINKS; i++) n += link_used(&s_links[i]);
    return n;
}

static int links_ready(void) {
    int n = 0;
    for (int i = 0; i < S2T_LINKS; i++) n += s_links[i].state == S2_LINK_READY;
    return n;
}

static link_t *link_by_pid(uint16_t pid) {
    for (int i = 0; i < S2T_LINKS; i++) {
        if (link_used(&s_links[i]) && s_links[i].pid == pid) return &s_links[i];
    }
    return NULL;
}

static link_t *first_used(void) {
    for (int i = 0; i < S2T_LINKS; i++) {
        if (link_used(&s_links[i])) return &s_links[i];
    }
    return NULL;
}

static link_t *first_ready(void) {
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_links[i].state == S2_LINK_READY) return &s_links[i];
    }
    return NULL;
}

// The link whose motion the controller reports: a Joy-Con pair's (R) when
// it is there.
static link_t *imu_link(void) {
    if (ctrl_is_joycon(s_type)) {
        link_t *r = link_by_pid(S2_PID_JOYCON2_R);
        if (r && r->state == S2_LINK_READY && s_type != CTRL_JOYCON_L) return r;
        link_t *l = link_by_pid(S2_PID_JOYCON2_L);
        if (l && l->state == S2_LINK_READY && s_type != CTRL_JOYCON_R) return l;
        return NULL;
    }
    return first_ready();
}

static void record_failure(uint8_t stage, uint8_t reason) {
    s_fail_stage = stage;
    s_fail_reason = reason;
    s_fail_time = platform_millis();
    LOG("s2: connection attempt failed at stage %u (reason 0x%02x)", stage, reason);
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

static void set_state(link_t *k, s2_link_state_t st) {
    if (st == k->state) return;
    k->state = st;
    LOG("s2: link %u -> %s", k->idx, s2_link_state_name(st));
    bool any = links_ready() > 0;
    if (any != s_any_ready) {
        s_any_ready = any;
        s2_link_hook_connection_changed(any);
    }
}

// ---------------------------------------------------------------------------
// Command queue (per link): one command in flight, matched to its response.
// ---------------------------------------------------------------------------
static bool cmd_submit(link_t *k, uint8_t cmd, uint8_t sub, const uint8_t *data, size_t len, cmd_cb_t cb,
                       uint32_t ctx) {
    if (k->cmd_count >= CMD_QUEUE_LEN) {
        LOG("s2: link %u command queue full (0x%02x/0x%02x)", k->idx, cmd, sub);
        return false;
    }
    cmd_t *c = &k->cmdq[(k->cmd_head + k->cmd_count) % CMD_QUEUE_LEN];
    size_t n = s2_build_command(c->buf, sizeof c->buf, cmd, sub, data, len);
    if (!n) return false;
    c->len = (uint8_t)n;
    c->cmd = cmd;
    c->sub = sub;
    c->cb = cb;
    c->ctx = ctx;
    c->tries = 0;
    k->cmd_count++;
    return true;
}

static bool cmd_submit_raw(link_t *k, const uint8_t *buf, size_t n, cmd_cb_t cb, uint32_t ctx) {
    if (k->cmd_count >= CMD_QUEUE_LEN || n > sizeof k->cmdq[0].buf) return false;
    cmd_t *c = &k->cmdq[(k->cmd_head + k->cmd_count) % CMD_QUEUE_LEN];
    memcpy(c->buf, buf, n);
    c->len = (uint8_t)n;
    c->cmd = buf[0];
    c->sub = buf[3];
    c->cb = cb;
    c->ctx = ctx;
    c->tries = 0;
    k->cmd_count++;
    return true;
}

static void cmd_finish(link_t *k, bool ok, const s2_response_t *rsp) {
    cmd_t c = k->cmdq[k->cmd_head];
    k->cmd_head = (k->cmd_head + 1) % CMD_QUEUE_LEN;
    k->cmd_count--;
    k->cmd_in_flight = false;
    if (c.cb) c.cb(k, ok, rsp, c.ctx);
}

static void cmd_pump(link_t *k) {
    if (!k->connected || k->cmd_count == 0) return;
    cmd_t *c = &k->cmdq[k->cmd_head];
    if (k->cmd_in_flight) {
        if (!platform_time_reached(k->cmd_deadline)) return;
        if (c->tries <= CMD_RETRIES) {
            LOG("s2: link %u command 0x%02x/0x%02x timed out, retrying", k->idx, c->cmd, c->sub);
            k->cmd_in_flight = false;
        } else {
            LOG("s2: link %u command 0x%02x/0x%02x failed", k->idx, c->cmd, c->sub);
            cmd_finish(k, false, NULL);
            return;
        }
    }
    s2t_write_result_t st = s2t_write(k->idx, S2T_CHAR_COMMAND, c->buf, c->len);
    if (st == S2T_WRITE_ERROR) {
        LOG("s2: command 0x%02x/0x%02x (%u bytes) could not be sent (ATT MTU %u)", c->cmd, c->sub, c->len,
            s2t_mtu(k->idx));
        cmd_finish(k, false, NULL);
        return;
    }
    if (st != S2T_WRITE_OK) return;   // TX buffers full: try again next pass
    c->tries++;
    k->cmd_in_flight = true;
    k->cmd_deadline = platform_deadline_ms(CMD_TIMEOUT_MS);
}

static void cmd_on_response(link_t *k, const uint8_t *data, uint16_t len) {
    s2_response_t rsp;
    if (!s2_parse_response(data, len, &rsp)) return;
    if (!k->cmd_in_flight || k->cmd_count == 0) return;
    cmd_t *c = &k->cmdq[k->cmd_head];
    if (rsp.cmd != c->cmd || rsp.sub != c->sub) {
        LOG("s2: link %u unexpected response 0x%02x/0x%02x", k->idx, rsp.cmd, rsp.sub);
        return;
    }
    cmd_finish(k, true, &rsp);
}

// ---------------------------------------------------------------------------
// Link lifetime
// ---------------------------------------------------------------------------
static void reset_link(link_t *k) {
    uint8_t idx = k->idx;
    memset(k, 0, sizeof *k);
    k->idx = idx;
    k->state = S2_LINK_OFF;
    k->led_sent = 0xFF;
    s2_default_stick_cal(&k->cal_l);
    s2_default_stick_cal(&k->cal_r);
    k->gc_trigger_neutral[0] = k->gc_trigger_neutral[1] = 30;
    k->gyro_lsb_per_dps = S2_GYRO_LSB_PER_DPS_A;
    k->info.power_level = -1;
    joycon_mouse_reset(&k->mouse);
}

static void drop_connection(link_t *k, const char *why) {
    LOG("s2: link %u dropping connection: %s", k->idx, why);
    if (k->connected) s2t_disconnect(k->idx);
}

static void disconnect_all(void) {
    if (s_connecting >= 0) {
        s2t_cancel_connect();
        reset_link(&s_links[s_connecting]);
        s_connecting = -1;
    }
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_links[i].connected) s2t_disconnect((uint8_t)i);
    }
}

// Scan while a controller (or the other half of a Joy-Con 2 pair) can still
// connect; one connection attempt at a time.
static void update_scan(void) {
    bool want = false;
    int used = links_used();
    if (s2t_ready() && s_connecting < 0) {
        if (used == 0) {
            want = true;
        } else if (used == 1) {
            const link_t *k = first_used();
            want = k->state == S2_LINK_READY && pid_is_joycon(k->pid) && !s_paused;
        }
    }
    // (Looking for the second Joy-Con 2 runs alongside the first one's
    // connection; the radio fits the scan around its connection events.
    // The other half's reconnection adverts are short, so no lower duty.)
    bool low = s_low_duty_scan;
    static bool scan_low;
    if (want && (!s_scanning || scan_low != low)) {
        s2t_start_scan(low);
        scan_low = low;
        if (!s_scanning) LOG("s2: scanning%s", used ? " for the other Joy-Con 2" : "");
        s_scanning = true;
    } else if (!want && s_scanning) {
        s2t_stop_scan();
        s_scanning = false;
    }
}

// ---------------------------------------------------------------------------
// Rumble
// ---------------------------------------------------------------------------
static rumble_sample_t s_rum_l[3], s_rum_r[3];
static int s_rum_nl, s_rum_nr;
static uint32_t s_rum_last_host_ms;

void s2_link_rumble_submit(const rumble_sample_t *left, int nl, const rumble_sample_t *right, int nr) {
    if (nl > 3) nl = 3;
    if (nr > 3) nr = 3;
    memcpy(s_rum_l, left, sizeof(rumble_sample_t) * (size_t)nl);
    memcpy(s_rum_r, right, sizeof(rumble_sample_t) * (size_t)nr);
    s_rum_nl = nl;
    s_rum_nr = nr;
    for (int i = 0; i < S2T_LINKS; i++) s_links[i].rum_dirty = true;
    s_rum_last_host_ms = now_ms();
}

static bool samples_active(const rumble_sample_t *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i].hi_amp > 0.0f || s[i].lo_amp > 0.0f) return true;
    }
    return false;
}

static float samples_peak(const rumble_sample_t *s, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; i++) m = fmaxf(m, fmaxf(s[i].hi_amp, s[i].lo_amp));
    return m;
}

// NSO GameCube controller: one classic motor, driven through its own
// vibration characteristic with 42-byte packets: 00, 0x50|seq, state (1 on,
// 0 off, 2 stop), zeros. Strength is the share of 12 ms slots it is on
// (error diffusion, as SDL does over USB).
#define GC_PACKET_LEN  42
#define GC_SLOT_MS     12

static bool gc_motor_write(link_t *k, uint8_t state) {
    uint8_t pkt[GC_PACKET_LEN];
    memset(pkt, 0, sizeof pkt);
    pkt[1] = (uint8_t)(0x50 | (k->rum_seq & 0x0F));
    pkt[2] = state;
    if (s2t_write(k->idx, S2T_CHAR_VIBRATION, pkt, sizeof pkt) != S2T_WRITE_OK) return false;
    k->rum_seq++;
    k->info.rumble_packets++;
    return true;
}

// duty 0..1; call often, it paces itself.
static void gc_motor_task(link_t *k, uint32_t now, float duty) {
    if (now - k->gc_last_send < GC_SLOT_MS) return;
    if (duty < 0.02f) {
        if (k->gc_running && gc_motor_write(k, 2)) {   // stop
            k->gc_running = false;
            k->gc_err = 0.0f;
            k->gc_last_send = now;
        }
        return;
    }
    if (duty > 1.0f) duty = 1.0f;
    float err = k->gc_err + duty;
    uint8_t state = err >= 1.0f ? 1 : 0;
    if (state) err -= 1.0f;
    if (gc_motor_write(k, state)) {
        k->gc_err = err;
        k->gc_running = true;
        k->gc_last_send = now;
    }
}

static float host_rumble_magnitude(uint32_t now) {
    float mag = fmaxf(samples_peak(s_rum_l, s_rum_nl), samples_peak(s_rum_r, s_rum_nr));
    mag *= (float)settings_tuning(&g_settings, s_type).rumble_strength / 100.0f;
    if (now - s_rum_last_host_ms > RUMBLE_IDLE_STOP_MS) mag = 0.0f;
    return mag;
}

// GameCube controller without the vibration characteristic: built-in samples.
static void gc_rumble_task(link_t *k, uint32_t now) {
    float mag = host_rumble_magnitude(now);
    bool rising = k->gc_last_mag < 0.06f && mag >= 0.06f;
    k->gc_last_mag = mag;
    if (mag < 0.06f) return;
    if (rising || now - k->gc_last_trigger_ms >= 220) {
        uint8_t d[4] = {(uint8_t)(mag >= 0.52f ? 0x02 : 0x03), 0, 0, 0};
        if (cmd_submit(k, S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0)) {
            k->gc_last_trigger_ms = now;
        }
    }
}

// The host's rumble a link plays: both motors on a Pro Controller; on a
// Joy-Con 2 pair each side its own motor; a single Joy-Con 2 the stronger one.
static void rumble_samples_for(const link_t *k, const rumble_sample_t **a, int *na, const rumble_sample_t **b,
                               int *nb) {
    *a = s_rum_l;
    *na = s_rum_nl;
    *b = s_rum_r;
    *nb = s_rum_nr;
    if (!pid_is_joycon(k->pid)) return;
    bool right;
    if (s_type == CTRL_JOYCON_PAIR) right = k->pid == S2_PID_JOYCON2_R;
    else right = samples_peak(s_rum_r, s_rum_nr) > samples_peak(s_rum_l, s_rum_nl);
    if (right) {
        *a = s_rum_r;
        *na = s_rum_nr;
    }
    *b = NULL;
    *nb = 0;
}

static uint16_t rumble_packet_len(const link_t *k) {
    return k->pid == S2_PID_PRO2 ? S2_RUMBLE_PRO_PACKET_LEN : 1 + S2_RUMBLE_BLOCK_LEN;
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

const char *s2_link_haptic_name(s2_haptic_t effect) {
    return effect < S2_HAPTIC_COUNT ? HAPTICS[effect].name : "?";
}

void s2_link_play_sample(uint8_t sample) {
    uint8_t d[4] = {sample, 0, 0, 0};
    for (int i = 0; i < S2T_LINKS; i++) {
        link_t *k = &s_links[i];
        if (k->state == S2_LINK_READY) cmd_submit(k, S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0);
    }
}

void s2_link_test_rumble(void) {
    s2_link_play_sample(0x01);   // built-in low frequency buzz
}

void s2_link_haptic(s2_haptic_t effect) {
    if (!links_ready() || effect >= S2_HAPTIC_COUNT) return;
    if (!HAPTICS[effect].steps) {
        s2_link_play_sample(HAPTICS[effect].sample);
        return;
    }
    s_hap = effect;
    s_hap_start = now_ms();
    for (int i = 0; i < S2T_LINKS; i++) {
        link_t *k = &s_links[i];
        k->hap_done = k->state != S2_LINK_READY;
        k->hap_last_send = s_hap_start - HAPTIC_PACKET_MS;
        if (k->state == S2_LINK_READY && !s2t_has_char(k->idx, S2T_CHAR_VIBRATION)) {
            // No HD actuator to play it on: the built-in buzz.
            uint8_t d[4] = {0x01, 0, 0, 0};
            cmd_submit(k, S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d, NULL, 0);
            k->hap_done = true;
        }
    }
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

// Plays the running effect on one link; true while it owns its actuators.
static bool haptic_task(link_t *k, uint32_t now) {
    if (s_hap < 0 || k->hap_done) return false;
    if (k->info.pid == S2_PID_GAMECUBE || k->pid == S2_PID_GAMECUBE) {
        // One motor: on while the effect's step is strong enough, then stop.
        rumble_sample_t smp;
        if (!haptic_at(now - s_hap_start, &smp)) {
            gc_motor_task(k, now, 0.0f);
            if (!k->gc_running) k->hap_done = true;
            return true;
        }
        gc_motor_task(k, now, fmaxf(smp.hi_amp, smp.lo_amp) > 0.3f ? 1.0f : 0.0f);
        return true;
    }
    if (now - k->hap_last_send < HAPTIC_PACKET_MS) return true;
    uint32_t t = now - s_hap_start;
    rumble_sample_t smp[3];
    bool more = false;
    for (int i = 0; i < 3; i++) more |= haptic_at(t + (uint32_t)i * HAPTIC_FRAME_MS, &smp[i]);
    // Fixed strength and real frequencies: the same feel whatever the rumble settings.
    s2_rumble_params_t p = {.translate_freq = true, .freq_slope = g_settings.rumble_freq_slope, .strength_pct = 100};
    uint8_t pkt[S2_RUMBLE_PRO_PACKET_LEN];
    pkt[0] = 0x00;
    s2_rumble_encode_block(k->rum_seq, smp, 3, &p, pkt + 1);
    s2_rumble_encode_block(k->rum_seq, smp, 3, &p, pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    if (s2t_write(k->idx, S2T_CHAR_VIBRATION, pkt, rumble_packet_len(k)) != S2T_WRITE_OK) return true;
    k->rum_seq++;
    k->hap_last_send = now;
    k->info.rumble_packets++;
    if (!more) {
        // That was the silent tail; hand the actuators back to the host's rumble.
        k->hap_done = true;
        k->rum_active = false;
        k->rum_dirty = true;
        k->rum_last_send_ms = now;
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

static void rumble_task(link_t *k, uint32_t now) {
    if (haptic_task(k, now) || !settings_tuning(&g_settings, s_type).rumble_enabled) return;
    if (k->pid == S2_PID_GAMECUBE) {
        if (s2t_has_char(k->idx, S2T_CHAR_VIBRATION)) gc_motor_task(k, now, host_rumble_magnitude(now));
        else gc_rumble_task(k, now);
        return;
    }
    if (!s2t_has_char(k->idx, S2T_CHAR_VIBRATION)) return;

    const rumble_sample_t *a, *b;
    int na, nb;
    rumble_samples_for(k, &a, &na, &b, &nb);
    bool host_idle = now - s_rum_last_host_ms > RUMBLE_IDLE_STOP_MS;
    bool want_active = !host_idle && (samples_active(a, na) || samples_active(b, nb));

    bool send = false;
    if (k->rum_dirty && (want_active || k->rum_active)) send = true;
    if (want_active && now - k->rum_last_send_ms >= RUMBLE_HOLD_MS) send = true;
    if (!want_active && k->rum_active) send = true;        // final stop packet
    if (!send || now - k->rum_last_send_ms < RUMBLE_MIN_GAP_MS) return;

    s2_rumble_params_t p = {
        .translate_freq = g_settings.rumble_freq_mode == RUMBLE_FREQ_TRANSLATE,
        .freq_slope = g_settings.rumble_freq_slope,
        .strength_pct = settings_tuning(&g_settings, s_type).rumble_strength,
    };
    uint8_t pkt[S2_RUMBLE_PRO_PACKET_LEN];
    pkt[0] = 0x00;
    if (want_active) {
        // A fresh host frame plays its own samples; a hold repeats the latest one.
        const rumble_sample_t *l = k->rum_dirty ? a : &a[na ? na - 1 : 0];
        const rumble_sample_t *r = b ? (k->rum_dirty ? b : &b[nb ? nb - 1 : 0]) : NULL;
        s2_rumble_encode_block(k->rum_seq, l, k->rum_dirty ? na : (na ? 1 : 0), &p, pkt + 1);
        s2_rumble_encode_block(k->rum_seq, r, r ? (k->rum_dirty ? nb : (nb ? 1 : 0)) : 0, &p,
                               pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    } else {
        s2_rumble_encode_block(k->rum_seq, NULL, 0, &p, pkt + 1);
        s2_rumble_encode_block(k->rum_seq, NULL, 0, &p, pkt + 1 + S2_RUMBLE_BLOCK_LEN);
    }
    if (s2t_write(k->idx, S2T_CHAR_VIBRATION, pkt, rumble_packet_len(k)) == S2T_WRITE_OK) {
        k->rum_seq++;
        k->rum_dirty = false;
        k->rum_active = want_active;
        k->rum_last_send_ms = now;
        k->info.rumble_packets++;
    }
}

// ---------------------------------------------------------------------------
// Player LEDs (the same on both Joy-Con 2)
// ---------------------------------------------------------------------------
static uint8_t s_led_wanted = 0x01;
static int s_led_override = -1;

void s2_link_set_player_leds(uint8_t pattern) {
    s_led_wanted = pattern & 0x0F;
}

void s2_link_set_led_override(int pattern) {
    s_led_override = pattern < 0 ? -1 : (pattern & 0x0F);
}

static void led_task(link_t *k) {
    uint8_t want = s_led_override >= 0 ? (uint8_t)s_led_override : s_led_wanted;
    if (want == k->led_sent) return;
    uint8_t d[8] = {want, 0, 0, 0, 0, 0, 0, 0};
    if (cmd_submit(k, S2_CMD_LEDS, S2_SUB_LEDS_SET_PATTERN, d, sizeof d, NULL, 0)) k->led_sent = want;
}

// ---------------------------------------------------------------------------
// Features: a Joy-Con 2 used as the mouse also reports its optical sensor.
// ---------------------------------------------------------------------------
#define FEATURE_FLAGS (S2_FEATURE_BUTTONS | S2_FEATURE_STICKS | S2_FEATURE_IMU | S2_FEATURE_UNK08 | S2_FEATURE_RUMBLE)

static uint8_t features_wanted(const link_t *k) {
    uint8_t f = FEATURE_FLAGS;
    // Mouse Mode: either Joy-Con 2 may become the mouse, so both report it.
    if (pid_is_joycon(k->pid) && settings_profile_mouse(settings_active(&g_settings, s_type), s_type)) {
        f |= S2_FEATURE_MOUSE;
    }
    return f;
}

static void features_enabled_cb(link_t *k, bool ok, const s2_response_t *rsp, uint32_t f) {
    (void)rsp;
    k->features_busy = false;
    if (ok) {
        k->features = (uint8_t)f;
        joycon_mouse_reset(&k->mouse);
        LOG("s2: link %u features 0x%02x", k->idx, k->features);
    } else {
        k->features = (uint8_t)f;   // don't retry forever
        LOG("s2: link %u could not change features", k->idx);
    }
}

static void features_task(link_t *k) {
    uint8_t want = features_wanted(k);
    if (k->features_busy || want == k->features) return;
    uint8_t d[4] = {want, 0, 0, 0};
    if (cmd_submit(k, S2_CMD_FEATURE, S2_SUB_FEATURE_SET_MASK, d, sizeof d, NULL, 0) &&
        cmd_submit(k, S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, d, sizeof d, features_enabled_cb, want)) {
        k->features_busy = true;
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

static void init_run(link_t *k);
static void after_init(link_t *k);

static void init_step_cb(link_t *k, bool ok, const s2_response_t *rsp, uint32_t step) {
    if ((int)step != k->init_step || k->phase != PH_INIT) return;
    if (!ok) {
        if (++k->init_tries <= 1) {
            init_run(k);
            return;
        }
        if (step == ST_FEATURE_MASK || step == ST_FEATURE_ENABLE) {
            drop_connection(k, "controller did not accept feature configuration");
            return;
        }
        if (step >= ST_PAIR_ADDR && step <= ST_PAIR_FINALIZE) {
            LOG("s2: pairing step %lu failed; continuing unpaired", (unsigned long)step);
            k->info.pairing_ok = false;
            k->init_step = ST_VIB_SAMPLE;
            k->init_tries = 0;
            init_run(k);
            return;
        }
        k->init_step++;
        k->init_tries = 0;
        init_run(k);
        return;
    }

    const uint8_t *payload;
    switch (step) {
    case ST_READ_INFO:
        if (s2_parse_memory_read(rsp, S2_ADDR_DEVICE_INFO, 0x40, &payload)) {
            memcpy(k->info.serial, payload + 2, 14);
            k->info.serial[14] = 0;
            uint16_t pid = (uint16_t)(payload[0x14] | (payload[0x15] << 8));
            if (pid) k->pid = pid;
            k->info.pid = k->pid;
            s2_link_hook_controller_colors(payload + 0x19);
            LOG("s2: link %u %s pid=%04x serial=%s", k->idx, pid_name(k->pid), k->pid, k->info.serial);
        }
        break;
    case ST_PAIR_ADDR:
        log_hex("s2: pair address reply", rsp->data, rsp->data_len > 9 ? 9 : rsp->data_len);
        break;
    case ST_PAIR_KEY:
        if (rsp->data_len >= 17) {
            uint8_t b1[16];
            reverse16(b1, rsp->data + 1);
            for (int i = 0; i < 16; i++) g_settings.ltk[i] = k->pair_a1[i] ^ b1[i];
        }
        break;
    case ST_PAIR_FINALIZE:
        LOG("s2: pairing complete");
        k->info.pairing_ok = true;
        k->info.paired_this_session = true;
        {
            bond_t old;
            if (settings_bond_add(&g_settings, k->peer, k->peer_type, k->pid, &old)) {
                LOG("s2: %d controllers paired already: forgot the oldest, %s", BOND_MAX, addr_str(old.addr));
            }
            LOG("s2: %d paired controller(s)", settings_bond_count(&g_settings));
        }
        settings_save_later();
        break;
    case ST_READ_CAL_L:
        if (s2_parse_memory_read(rsp, S2_ADDR_FACTORY_STICK_L, 0x40, &payload)) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 0x28, &c);
            if (c.valid) {
                k->cal_l = c;
                k->cal_l_read = true;
            }
        }
        break;
    case ST_READ_CAL_R:
        if (s2_parse_memory_read(rsp, S2_ADDR_FACTORY_STICK_R, 0x40, &payload)) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 0x28, &c);
            if (c.valid) {
                k->cal_r = c;
                k->cal_r_read = true;
            }
        }
        break;
    case ST_READ_USER_L:
        if (s2_parse_memory_read(rsp, S2_ADDR_USER_STICK_L, 0x40, &payload) && payload[0] == 0xb2 &&
            payload[1] == 0xa1) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 2, &c);
            if (c.valid) {
                k->cal_l = c;
                k->cal_l_read = true;
            }
        }
        break;
    case ST_READ_USER_R:
        if (s2_parse_memory_read(rsp, S2_ADDR_USER_STICK_R, 0x40, &payload) && payload[0] == 0xb2 &&
            payload[1] == 0xa1) {
            s2_stick_cal_t c;
            s2_parse_stick_cal(payload + 2, &c);
            if (c.valid) {
                k->cal_r = c;
                k->cal_r_read = true;
            }
        }
        LOG("s2: stick cal L c=%u,%u +%u,%u -%u,%u  R c=%u,%u +%u,%u -%u,%u", k->cal_l.center[0],
            k->cal_l.center[1], k->cal_l.max[0], k->cal_l.max[1], k->cal_l.min[0], k->cal_l.min[1],
            k->cal_r.center[0], k->cal_r.center[1], k->cal_r.max[0], k->cal_r.max[1], k->cal_r.min[0],
            k->cal_r.min[1]);
        break;
    case ST_READ_GC_TRIGGERS:
        if (s2_parse_memory_read(rsp, S2_ADDR_GC_TRIGGERS, 0x02, &payload) && payload[0] != 0x00 &&
            payload[0] != 0xFF) {
            k->gc_trigger_neutral[0] = payload[0];
            k->gc_trigger_neutral[1] = payload[1];
        }
        break;
    case ST_LEDS:
        k->led_sent = s_led_wanted;
        break;
    case ST_FEATURE_ENABLE:
        k->features = features_wanted(k);
        break;
    default:
        break;
    }
    k->init_step++;
    k->init_tries = 0;
    init_run(k);
}

static void init_submit(link_t *k, uint8_t cmd, uint8_t sub, const uint8_t *d, size_t n) {
    if (!cmd_submit(k, cmd, sub, d, n, init_step_cb, (uint32_t)k->init_step)) {
        drop_connection(k, "command queue");
    }
}

static void init_submit_read(link_t *k, uint32_t addr, uint8_t len) {
    uint8_t buf[32];
    size_t n = s2_build_memory_read(buf, sizeof buf, addr, len);
    if (!cmd_submit_raw(k, buf, n, init_step_cb, (uint32_t)k->init_step)) drop_connection(k, "command queue");
}

static void init_run(link_t *k) {
    for (;;) {
        switch (k->init_step) {
        case ST_UNK07:
            init_submit(k, S2_CMD_UNK07, 0x01, NULL, 0);
            return;
        case ST_READ_INFO:
            init_submit_read(k, S2_ADDR_DEVICE_INFO, 0x40);
            return;
        case ST_UNK16:
            init_submit(k, S2_CMD_UNK16, 0x01, NULL, 0);
            return;
        case ST_PAIR_ADDR: {
            if (!k->need_pairing) {
                k->init_step = ST_VIB_SAMPLE;
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
            init_submit(k, S2_CMD_PAIR, S2_SUB_PAIR_SET_ADDRESS, d, sizeof d);
            return;
        }
        case ST_PAIR_KEY: {
            uint8_t d[17];
            for (int i = 0; i < 16; i += 4) {
                uint32_t r = platform_random32();
                memcpy(k->pair_a1 + i, &r, 4);
            }
            d[0] = 0x00;
            reverse16(d + 1, k->pair_a1);
            init_submit(k, S2_CMD_PAIR, S2_SUB_PAIR_EXCHANGE_KEY, d, sizeof d);
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
            init_submit(k, S2_CMD_PAIR, S2_SUB_PAIR_CONFIRM_LTK, d, sizeof d);
            return;
        }
        case ST_PAIR_FINALIZE: {
            uint8_t d[1] = {0x00};
            init_submit(k, S2_CMD_PAIR, S2_SUB_PAIR_FINALIZE, d, sizeof d);
            return;
        }
        case ST_VIB_SAMPLE: {
            uint8_t d[4] = {0x03, 0, 0, 0};   // the "connected" click
            init_submit(k, S2_CMD_VIBRATION, S2_SUB_VIB_PLAY_SAMPLE, d, sizeof d);
            return;
        }
        case ST_LEDS: {
            uint8_t d[8] = {s_led_wanted, 0, 0, 0, 0, 0, 0, 0};
            init_submit(k, S2_CMD_LEDS, S2_SUB_LEDS_SET_PATTERN, d, sizeof d);
            return;
        }
        case ST_FEATURE_MASK: {
            uint8_t d[4] = {features_wanted(k), 0, 0, 0};
            init_submit(k, S2_CMD_FEATURE, S2_SUB_FEATURE_SET_MASK, d, sizeof d);
            return;
        }
        // Both sticks' calibration, also on a Joy-Con 2 (which has one stick
        // and may keep its calibration in either place; see after_init()).
        case ST_READ_CAL_L:
            init_submit_read(k, S2_ADDR_FACTORY_STICK_L, 0x40);
            return;
        case ST_READ_CAL_R:
            init_submit_read(k, S2_ADDR_FACTORY_STICK_R, 0x40);
            return;
        case ST_READ_USER_L:
            init_submit_read(k, S2_ADDR_USER_STICK_L, 0x40);
            return;
        case ST_READ_USER_R:
            init_submit_read(k, S2_ADDR_USER_STICK_R, 0x40);
            return;
        case ST_READ_GC_TRIGGERS:
            if (k->pid != S2_PID_GAMECUBE) {
                k->init_step++;
                continue;
            }
            init_submit_read(k, S2_ADDR_GC_TRIGGERS, 0x02);
            return;
        case ST_VIB_DATA: {
            // Sent by the console before enabling features (contents not understood).
            static const uint8_t d[20] = {0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                                          0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            init_submit(k, S2_CMD_VIBRATION, S2_SUB_VIB_SEND_DATA, d, sizeof d);
            return;
        }
        case ST_FEATURE_ENABLE: {
            uint8_t d[4] = {features_wanted(k), 0, 0, 0};
            init_submit(k, S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, d, sizeof d);
            return;
        }
        case ST_DONE:
        default:
            after_init(k);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Link phases driven by the transport
// ---------------------------------------------------------------------------
static void phase(link_t *k, link_phase_t p) {
    k->phase = p;
    k->phase_deadline = platform_deadline_ms(GATT_PHASE_TIMEOUT_MS);
}

static void after_init(link_t *k) {
    // A Joy-Con 2's one stick: the calibration found, wherever it was (an
    // (R) was seen with nothing at the right stick's address).
    if (k->pid == S2_PID_JOYCON2_R && !k->cal_r_read && k->cal_l_read) {
        k->cal_r = k->cal_l;
        LOG("s2: link %u stick calibration from the left stick's place", k->idx);
    } else if (k->pid == S2_PID_JOYCON2_L && !k->cal_l_read && k->cal_r_read) {
        k->cal_l = k->cal_r;
        LOG("s2: link %u stick calibration from the right stick's place", k->idx);
    } else if (pid_is_joycon(k->pid) && !k->cal_l_read && !k->cal_r_read) {
        LOG("s2: link %u has no stick calibration: using defaults", k->idx);
    }
    // (The console also writes 0x85 0x00 to an undocumented "report rate"
    // descriptor; the working Linux implementation doesn't, so neither do we.)
    phase(k, PH_INPUT_ENABLE);
    s2t_enable_input(k->idx);
}

static void on_input_notification(link_t *k, const uint8_t *value, uint16_t len) {
    s2_input_t in;
    if (!s2_parse_input_report(value, len, &in)) return;
    k->input = in;
    k->input_seq++;
    k->info.reports++;
    k->rate_count++;
    k->info.battery_mv = in.battery_mv;
    k->info.charge_state = in.charge_state;

    // Gyro range auto detection: if the IMU timestamp advances in real
    // microseconds the gyro is +-2000 dps over int16; otherwise use the
    // alternative scale SDL found on such controllers.
    if (!k->imu_detected && in.imu_timestamp) {
        if (k->imu_samples == 0) {
            k->imu_first_ts = in.imu_timestamp;
            k->imu_first_time = platform_millis();
        }
        k->imu_samples++;
        uint32_t elapsed_ms = platform_millis() - k->imu_first_time;
        if (elapsed_ms > GYRO_DETECT_MS) {
            float ratio = (float)(uint32_t)(in.imu_timestamp - k->imu_first_ts) / ((float)elapsed_ms * 1000.0f);
            bool micro = ratio > 0.85f && ratio < 1.15f;
            k->imu_detected = true;
            k->info.gyro_range_detected = micro ? GYRO_RANGE_16_4 : GYRO_RANGE_14_3;
            LOG("s2: link %u IMU timestamp ratio %.3f -> gyro %s LSB/dps", k->idx, (double)ratio,
                micro ? "16.4" : "14.3");
        }
    }
    uint8_t range = g_settings.gyro_range;
    if (range == GYRO_RANGE_AUTO) range = k->imu_detected ? k->info.gyro_range_detected : GYRO_RANGE_16_4;
    k->gyro_lsb_per_dps = range == GYRO_RANGE_14_3 ? S2_GYRO_LSB_PER_DPS_B : S2_GYRO_LSB_PER_DPS_A;

    if (k->features & S2_FEATURE_MOUSE) {
        int32_t dx, dy;
        joycon_mouse_delta(&k->mouse, in.mouse_x, in.mouse_y, &dx, &dy);
        k->mouse_dx += dx;
        k->mouse_dy += dy;
    }

    if (s_cal_active && k == imu_link()) {
        for (int i = 0; i < 3; i++) s_cal_sum[i] += in.gyro[i];
        s_cal_n++;
    }
}

// ---------------------------------------------------------------------------
// Which controller type the connected controller(s) are
// ---------------------------------------------------------------------------
// The other Joy-Con 2 of `pid` is paired with the dongle.
static bool partner_bonded(uint16_t pid) {
    uint16_t other = pid == S2_PID_JOYCON2_L ? S2_PID_JOYCON2_R : S2_PID_JOYCON2_L;
    for (int i = 0; i < BOND_MAX; i++) {
        if (g_settings.bonds[i].used && g_settings.bonds[i].pid == other) return true;
    }
    return false;
}

static void set_type(ctrl_type_t t) {
    if (t == s_type) return;
    LOG("s2: controller is now %s", ctrl_type_name(t));
    s_type = t;
}

// A pair that has run with only one Joy-Con 2 for a while, its partner
// never having connected: that one is used on its own (sideways).
static void half_pair_task(void) {
    // Single Joy-Con 2 turned off (on the page) while one is used alone: it
    // becomes its half of the pair.
    if (!g_settings.joycon_single && (s_type == CTRL_JOYCON_L || s_type == CTRL_JOYCON_R)) {
        set_type(CTRL_JOYCON_PAIR);
        app_controller_type(CTRL_JOYCON_PAIR);
        return;
    }
    if (!g_settings.joycon_single) return;
    if (s_type != CTRL_JOYCON_PAIR || s_pair_complete || links_used() != 1) return;
    link_t *k = first_ready();
    if (!k || !pid_is_joycon(k->pid) || s_connecting >= 0) return;
    if (!s_half_since) s_half_since = now_ms() | 1u;
    if (now_ms() - s_half_since < HALF_PAIR_MS) return;
    ctrl_type_t t = type_of_pid(k->pid);
    LOG("s2: the other Joy-Con 2 didn't connect: using the %s on its own", pid_name(k->pid));
    s_half_since = 0;
    set_type(t);
    app_controller_type(t);   // may restart into its profile's mode
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
    update_scan();
}

static bool supported_pid(uint16_t pid) {
    return pid == S2_PID_PRO2 || pid == S2_PID_GAMECUBE || pid_is_joycon(pid);
}

void s2c_on_advertisement(const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                          const uint8_t *adv_data, uint16_t adv_len) {
    if (!s_scanning || s_connecting >= 0) return;
    const uint8_t *mdata;
    uint8_t mlen;
    if (!adv_find_manufacturer(adv_data, adv_len, &mdata, &mlen)) return;
    s2_adv_info_t adv;
    if (!s2_parse_manufacturer_data(mdata, mlen, &adv)) return;
    s_last_rssi = rssi;

    if (!supported_pid(adv.pid)) {
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
    bool is_bonded = settings_bond_find(&g_settings, addr) >= 0;

    if (s_paused) {
        // While the USB host sleeps we only watch for a bonded controller
        // waking up (a button press) so the host can be woken. Right after we
        // dropped the link the controller keeps advertising on its own for a
        // while, so those adverts are ignored.
        if (for_us && is_bonded && platform_time_reached(s_pause_quiet_until) && platform_time_reached(s_seen_hook_next)) {
            s_seen_hook_next = platform_deadline_ms(1000);
            s2_link_hook_controller_seen();
        }
        return;
    }

    // One controller at a time; the only second one is the other half of a
    // Joy-Con 2 pair (an (L) with an (R)), once the first is ready.
    int used = links_used();
    const link_t *other = first_used();
    if (used >= S2T_LINKS) return;
    if (used == 1) {
        if (!pid_is_joycon(adv.pid) || !pid_is_joycon(other->pid) || adv.pid == other->pid) {
            // Asked to pair (the window is open) and this isn't the other
            // half: the kept Joy-Con 2 makes room for it.
            if (adv.pairing_mode && s_pair_open && g_settings.pair_button && !platform_time_reached(s_pair_until) &&
                memcmp(other->peer, addr, 6) != 0) {
                LOG("s2: %s in pairing mode: letting the %s go", pid_name(adv.pid), pid_name(other->pid));
                disconnect_all();
            }
            return;
        }
        if (other->state != S2_LINK_READY || memcmp(other->peer, addr, 6) == 0) return;
    }

    // Pairing only on request (Sync button / page): pairing-mode adverts are
    // ignored outside the window, and inside it nothing else is accepted.
    if (s_pair_open && platform_time_reached(s_pair_until)) {
        s_pair_open = false;
        LOG("s2: pairing window closed (timed out)");
    }
    if (g_settings.pair_button) {
        // (Completing a Joy-Con 2 pair is always fine: the other half
        // reconnecting while the window is open, e.g. after pairing one side.)
        if (s_pair_open && !adv.pairing_mode && used == 0) return;
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

    bool need_pairing;
    if (adv.pairing_mode) {
        LOG("s2: %s %s in pairing mode", pid_name(adv.pid), addr_str(addr));
        need_pairing = true;
    } else if (for_us) {
        // A controller that still remembers us but that we don't (pairing
        // forgotten, or dropped as the oldest of too many) must be paired
        // again with Sync, like a new one.
        if (!is_bonded) {
            if (platform_time_reached(s_unbonded_log)) {
                s_unbonded_log = platform_deadline_ms(10000);
                LOG("s2: controller %s isn't paired with the dongle: hold Sync on it to pair", addr_str(addr));
            }
            return;
        }
        need_pairing = false;
    } else {
        return;   // reconnecting to another host (e.g. a console)
    }

    // Which controller this makes: the second Joy-Con 2 completes a pair; a
    // first one is half of a pair when that is how it was used last and its
    // partner is paired too (the partner usually follows), else sideways.
    // (Single Joy-Con 2 not allowed: always the pair, even a half one.)
    ctrl_type_t t;
    if (used == 1) t = CTRL_JOYCON_PAIR;
    else if (pid_is_joycon(adv.pid) && !g_settings.joycon_single) t = CTRL_JOYCON_PAIR;
    else if (pid_is_joycon(adv.pid))
        t = g_settings.last_ctrl == CTRL_JOYCON_PAIR && partner_bonded(adv.pid) ? CTRL_JOYCON_PAIR
                                                                               : type_of_pid(adv.pid);
    else t = type_of_pid(adv.pid);

    // Each controller type has its own USB mode; this may restart the dongle
    // into it first (the controller connects after the restart). A controller
    // in pairing mode is paired first (a restart would close the pairing
    // window); the switch then happens once it is ready.
    if (!adv.pairing_mode && app_controller_type(t)) return;

    link_t *k = NULL;
    for (int i = 0; i < S2T_LINKS && !k; i++) {
        if (!link_used(&s_links[i])) k = &s_links[i];
    }
    if (!k) return;
    s_type_before = s_type;
    if (used == 0) {
        s_pair_complete = false;
        s_half_since = 0;
    }
    set_type(t);

    s2t_stop_scan();
    s_scanning = false;
    reset_link(k);
    memcpy(k->peer, addr, 6);
    k->peer_type = addr_type;
    k->pid = adv.pid;
    k->info.pid = adv.pid;
    k->need_pairing = need_pairing;
    LOG("s2: connecting to %s %s on link %u (type %u, pairing=%d)", pid_name(adv.pid), addr_str(addr), k->idx,
        addr_type, need_pairing);
    if (!s2t_connect(k->idx, addr, addr_type)) {
        record_failure(1, 0xFE);
        reset_link(k);
        if (used) set_type(s_type_before);
        update_scan();
        return;
    }
    s_connecting = k->idx;
    set_state(k, S2_LINK_CONNECTING);
    k->phase_deadline = platform_deadline_ms(CONNECT_TIMEOUT_MS);
}

static link_t *link_at(uint8_t link) {
    return link < S2T_LINKS ? &s_links[link] : NULL;
}

// A second Joy-Con 2 didn't make it: the first carries on as it was (and
// the dongle starts as that next time, not as the pair).
static void revert_type(void) {
    set_type(s_type_before);
    if (g_settings.last_ctrl != s_type_before) {
        g_settings.last_ctrl = (uint8_t)s_type_before;
        settings_save_later();
    }
}

// A connection attempt ended without a link.
static void connect_ended(link_t *k) {
    bool second = links_used() > 1;
    reset_link(k);
    if (s_connecting == k->idx) s_connecting = -1;
    if (second) revert_type();
    update_scan();
}

void s2c_on_link_up(uint8_t link, uint16_t conn_interval) {
    link_t *k = link_at(link);
    if (!k || k->state != S2_LINK_CONNECTING) {
        // A connection that completed after we gave up on it (timeout,
        // pause, pairing window): let it go, or it would hold a link.
        LOG("s2: link %u connected after it was given up: disconnecting", link);
        s2t_disconnect(link);
        return;
    }
    s_connecting = -1;
    k->connected = true;
    k->info.conn_interval = conn_interval;
    memcpy(k->info.addr, k->peer, 6);
    k->info.paired_this_session = false;
    LOG("s2: link %u connected, interval %u", k->idx, conn_interval);
    set_state(k, S2_LINK_DISCOVERING);
    phase(k, PH_DISCOVERY);
    update_scan();
}

void s2c_on_connect_failed(uint8_t link, uint8_t reason) {
    link_t *k = link_at(link);
    if (!k || k->state != S2_LINK_CONNECTING) return;
    LOG("s2: link %u connection failed (0x%02x)", k->idx, reason);
    record_failure(1, reason);
    connect_ended(k);
}

void s2c_on_gatt_ready(uint8_t link, bool ok, const char *error) {
    link_t *k = link_at(link);
    if (!k || k->phase != PH_DISCOVERY) return;
    if (!ok) {
        drop_connection(k, error ? error : "GATT discovery failed");
        return;
    }
    phase(k, PH_INIT);
    set_state(k, S2_LINK_INITIALISING);
    k->init_step = ST_UNK07;
    k->init_tries = 0;
    init_run(k);
}

void s2c_on_command_response(uint8_t link, const uint8_t *data, uint16_t len) {
    link_t *k = link_at(link);
    if (k) cmd_on_response(k, data, len);
}

void s2c_on_input_report(uint8_t link, const uint8_t *data, uint16_t len) {
    link_t *k = link_at(link);
    if (k && (k->phase == PH_READY || k->phase == PH_INPUT_ENABLE)) on_input_notification(k, data, len);
}

void s2c_on_input_enabled(uint8_t link, bool ok) {
    link_t *k = link_at(link);
    if (!k || k->phase != PH_INPUT_ENABLE) return;
    if (!ok) {
        drop_connection(k, "could not enable input notifications");
        return;
    }
    phase(k, PH_READY);
    k->info.mtu = s2t_mtu(k->idx);
    LOG("s2: link %u %s ready (ATT MTU %u)", k->idx, pid_name(k->pid), k->info.mtu);
    k->rate_window = platform_deadline_ms(1000);
    k->rate_count = 0;
    set_state(k, S2_LINK_READY);
    power_start(k);
    if (links_ready() == S2T_LINKS) s_pair_complete = true;
    // A controller paired: the pairing window closes (the other half of a
    // Joy-Con 2 pair is paired with the window opened again). One that only
    // reconnected closes it unless it is a Joy-Con 2 still waiting for its
    // other half.
    if (s_pair_open && (k->need_pairing || !(pid_is_joycon(k->pid) && links_ready() < S2T_LINKS))) {
        s_pair_open = false;
        LOG("s2: pairing window closed (controller connected)");
    }
    // Normally done before connecting; after pairing, this may restart into
    // the controller type's mode (the controller then reconnects).
    app_controller_type(s_type);
    update_scan();
}

void s2c_on_conn_interval(uint8_t link, uint16_t conn_interval) {
    link_t *k = link_at(link);
    if (!k) return;
    k->info.conn_interval = conn_interval;
    LOG("s2: link %u connection interval now %u", k->idx, conn_interval);
}

void s2c_on_disconnected(uint8_t link, uint8_t reason) {
    link_t *k = link_at(link);
    if (!k || (!k->connected && k->state != S2_LINK_CONNECTING)) return;
    LOG("s2: link %u disconnected (reason 0x%02x)", k->idx, reason);
    if (k->state == S2_LINK_CONNECTING) record_failure(1, reason);
    else if (k->state == S2_LINK_DISCOVERING) record_failure(2, reason);
    else if (k->state == S2_LINK_INITIALISING) record_failure(k->init_step <= ST_PAIR_FINALIZE ? 3 : 4, reason);
    bool was_ready = k->state == S2_LINK_READY;
    if (s_connecting == k->idx) s_connecting = -1;
    // A second Joy-Con 2 that never got ready: the first carries on as before.
    bool second_failed = !was_ready && links_used() > 1 && links_ready() >= 1;
    s_last_info = k->info;
    set_state(k, S2_LINK_OFF);
    reset_link(k);
    if (second_failed) revert_type();
    if (!links_used()) {
        s_pair_complete = false;
        s_half_since = 0;
    }
    refresh_merged();
    update_scan();
}

// ---------------------------------------------------------------------------
// Controller's own battery level. Report 0x05 (the one we use) only has the
// battery voltage, which doesn't map well to a level; the controller-specific
// report has the level the console shows (Power Info). Subscribing to it
// for good would double the radio traffic, so it is read once right after
// connecting and then once a minute: subscribe, take one report, unsubscribe.
// If input reports stop meanwhile, or it keeps failing, polling stops for
// this connection and the voltage estimate (battery.c) stays.
// ---------------------------------------------------------------------------
#define POWER_FIRST_MS  3000
#define POWER_EVERY_MS  60000
#define POWER_CHECK_MS  500

static void power_start(link_t *k) {
    k->power_off = k->power_busy = k->power_check = false;
    k->power_fails = 0;
    // A second controller polls a little later than the first.
    k->power_next = platform_deadline_ms(POWER_FIRST_MS + 1500u * k->idx);
    k->info.power_level = -1;
    k->info.power_info = 0;
}

static void power_give_up(link_t *k, const char *why) {
    k->power_off = true;
    LOG("s2: link %u battery level unavailable (%s): using the voltage estimate", k->idx, why);
}

static void power_task(link_t *k) {
    if (k->power_check && platform_time_reached(k->power_check_at)) {
        // Input reports must still be arriving after the poll.
        k->power_check = false;
        if (k->info.reports == k->power_reports) {
            power_give_up(k, "input reports stopped");
            s2t_enable_input(k->idx);
        }
    }
    if (k->power_off || k->power_busy || !platform_time_reached(k->power_next)) return;
    // One poll at a time across links (they share the radio). A poll whose
    // answer never came (a lost event) doesn't block the others for good.
    for (int i = 0; i < S2T_LINKS; i++) {
        link_t *o = &s_links[i];
        if (o->power_busy && platform_time_reached(o->power_busy_until)) {
            o->power_busy = false;
            o->power_next = platform_deadline_ms(POWER_EVERY_MS);
        }
        if (o->power_busy) return;
    }
    if (!s2t_poll_power(k->idx)) {
        k->power_next = platform_deadline_ms(5000);
        if (++k->power_fails >= 5) power_give_up(k, "no such report");
        return;
    }
    k->power_busy = true;
    k->power_busy_until = platform_deadline_ms(5000);
}

// The battery shown is the lower one of a Joy-Con 2 pair.
static void battery_from_links(void) {
    const link_t *low = NULL;
    for (int i = 0; i < S2T_LINKS; i++) {
        const link_t *k = &s_links[i];
        if (k->state != S2_LINK_READY || k->info.power_level < 0) continue;
        if (!low || k->info.power_level < low->info.power_level) low = k;
    }
    if (!low) return;
    uint8_t info = low->info.power_info;
    battery_set_level(&g_battery, (uint8_t)low->info.power_level, (info & 1) != 0, (info & 2) != 0);
}

void s2c_on_power_info(uint8_t link, bool ok, uint8_t info) {
    link_t *k = link_at(link);
    if (!k || !k->power_busy) return;
    k->power_busy = false;
    k->power_next = platform_deadline_ms(POWER_EVERY_MS);
    k->power_check = true;
    k->power_check_at = platform_deadline_ms(POWER_CHECK_MS);
    k->power_reports = k->info.reports;
    if (!ok) {
        if (++k->power_fails >= 3) power_give_up(k, "no answer");
        return;
    }
    k->power_fails = 0;
    int8_t level = (int8_t)((info >> 2) & 0x0F);
    if (level != k->info.power_level || info != k->info.power_info) {
        LOG("s2: link %u battery level %d/9%s%s (power info 0x%02x)", k->idx, level,
            info & 1 ? ", external power" : "", info & 2 ? ", charging" : "", info);
    }
    k->info.power_level = level;
    k->info.power_info = info;
    battery_from_links();
}

// ---------------------------------------------------------------------------
// The controller as one: a single link's input, or a Joy-Con 2 pair merged.
// ---------------------------------------------------------------------------
static void refresh_merged(void) {
    uint32_t seq = 0;
    for (int i = 0; i < S2T_LINKS; i++) seq += s_links[i].input_seq;
    link_t *one = first_ready();
    memset(&s_map, 0, sizeof s_map);
    s_map.type = s_type;
    s2_default_stick_cal(&s_map.cal_l);
    s2_default_stick_cal(&s_map.cal_r);
    s_map.gc_trigger_neutral[0] = s_map.gc_trigger_neutral[1] = 30;
    s_map.gyro_lsb_per_dps = S2_GYRO_LSB_PER_DPS_A;
    if (ctrl_is_joycon(s_type)) {
        joycon_side_t l, r;
        memset(&l, 0, sizeof l);
        memset(&r, 0, sizeof r);
        link_t *kl = link_by_pid(S2_PID_JOYCON2_L), *kr = link_by_pid(S2_PID_JOYCON2_R);
        if (kl && kl->state == S2_LINK_READY) {
            l.present = true;
            l.in = kl->input;
            l.cal = kl->cal_l;
            l.gyro_lsb_per_dps = kl->gyro_lsb_per_dps;
        }
        if (kr && kr->state == S2_LINK_READY) {
            r.present = true;
            r.in = kr->input;
            r.cal = kr->cal_r;
            r.gyro_lsb_per_dps = kr->gyro_lsb_per_dps;
        }
        joycon_merge(s_type, &l, &r, &s_merged, &s_map);
    } else if (one) {
        s_merged = one->input;
        s_map.cal_l = one->cal_l;
        s_map.cal_r = one->cal_r;
        s_map.is_gamecube = one->pid == S2_PID_GAMECUBE;
        memcpy(s_map.gc_trigger_neutral, one->gc_trigger_neutral, 2);
        s_map.gyro_lsb_per_dps = one->gyro_lsb_per_dps;
    } else {
        memset(&s_merged, 0, sizeof s_merged);
    }
    s_merged_seq = seq;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void s2_link_set_low_duty_scan(bool low) {
    if (low == s_low_duty_scan) return;
    s_low_duty_scan = low;
    update_scan();
}

bool s2_link_last_failure(uint8_t *stage, uint8_t *reason, uint32_t *age_ms) {
    if (!s_fail_stage) return false;
    *stage = s_fail_stage;
    *reason = s_fail_reason;
    *age_ms = platform_millis() - s_fail_time;
    return true;
}

void s2_link_init(void) {
    for (int i = 0; i < S2T_LINKS; i++) {
        s_links[i].idx = (uint8_t)i;
        reset_link(&s_links[i]);
    }
    s_type = settings_boot_ctrl(&g_settings);
    s_last_info.power_level = -1;
    refresh_merged();
    s2t_init();
}

void s2_link_task(void) {
    s2t_task();
    uint32_t now = now_ms();

    if (s_test_on) {
        // Fed like host rumble; it fades out by itself once this stops.
        if ((int32_t)(now - s_test_until) >= 0) s_test_on = false;
        else s2_link_rumble_submit(&s_test_l, 1, &s_test_r, 1);
    }

    for (int i = 0; i < S2T_LINKS; i++) {
        link_t *k = &s_links[i];
        // Connection / discovery watchdogs.
        if (k->state == S2_LINK_CONNECTING && platform_time_reached(k->phase_deadline)) {
            LOG("s2: link %u connect timed out", k->idx);
            record_failure(1, 0xFF);
            s2t_cancel_connect();
            connect_ended(k);
            continue;
        }
        if (k->connected && (k->phase == PH_DISCOVERY || k->phase == PH_INPUT_ENABLE) &&
            platform_time_reached(k->phase_deadline)) {
            k->phase_deadline = platform_deadline_ms(GATT_PHASE_TIMEOUT_MS);
            drop_connection(k, k->phase == PH_DISCOVERY ? "GATT discovery timeout" : "input enable timeout");
        }
        if (k->state == S2_LINK_READY && platform_time_reached(k->rate_window)) {
            k->info.report_rate_hz = (float)k->rate_count;
            k->rate_count = 0;
            k->rate_window = platform_deadline_ms(1000);
        }
        cmd_pump(k);
        if (k->state == S2_LINK_READY) {
            led_task(k);
            features_task(k);
            rumble_task(k, now);
            power_task(k);
        } else {
            k->gc_running = false;
            k->gc_err = 0.0f;
        }
    }
    // The effect is over once every link has played it.
    if (s_hap >= 0) {
        bool done = true;
        for (int i = 0; i < S2T_LINKS; i++) done &= s_links[i].hap_done || s_links[i].state != S2_LINK_READY;
        if (done) s_hap = -1;
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

    half_pair_task();
    update_scan();
    refresh_merged();
}

s2_link_state_t s2_link_state(void) {
    s2_link_state_t st = S2_LINK_OFF;
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_links[i].state > st) st = s_links[i].state;
    }
    if (st == S2_LINK_OFF && s_scanning) st = S2_LINK_SCANNING;
    return st;
}

ctrl_type_t s2_link_ctrl_type(void) {
    return s_type;
}

static void fill_info(const link_t *k, s2_link_info_t *out) {
    *out = k->info;
    out->state = k->state;
    out->gyro_cal_busy = s_cal_active;
    out->last_rssi = s_last_rssi;
    if (!k->imu_detected) out->gyro_range_detected = 0;
}

void s2_link_get_info(s2_link_info_t *out) {
    // The controller as a whole: its first ready link (else the one
    // connecting), with the shared state.
    const link_t *k = first_ready();
    if (!k) k = first_used();
    if (k) {
        fill_info(k, out);
    } else {
        // Nothing connected: the last controller's details, as before.
        *out = s_last_info;
        out->power_level = -1;
        out->report_rate_hz = 0;
        out->last_rssi = s_last_rssi;
    }
    out->state = s2_link_state();
    out->gyro_cal_busy = s_cal_active;
    const link_t *m = imu_link();
    if (m) out->gyro_range_detected = m->imu_detected ? m->info.gyro_range_detected : 0;
}

bool s2_link_get_link_info(int link, s2_link_info_t *out) {
    if (link < 0 || link >= S2T_LINKS || !link_used(&s_links[link])) return false;
    fill_info(&s_links[link], out);
    return true;
}

bool s2_link_get_input(s2_input_t *out, uint32_t *seq) {
    if (!links_ready()) return false;
    *out = s_merged;
    if (seq) *seq = s_merged_seq;
    return true;
}

const mapping_ctx_t *s2_link_mapping_ctx(void) {
    return &s_map;
}

bool s2_link_mouse_take(uint16_t pid, int32_t *dx, int32_t *dy) {
    link_t *k = link_by_pid(pid);
    if (!k || k->state != S2_LINK_READY) {
        *dx = *dy = 0;
        return false;
    }
    *dx = k->mouse_dx;
    *dy = k->mouse_dy;
    k->mouse_dx = k->mouse_dy = 0;
    return true;
}

bool s2_link_side_input(uint16_t pid, s2_input_t *out, s2_stick_cal_t *cal) {
    link_t *k = link_by_pid(pid);
    if (!k || k->state != S2_LINK_READY) return false;
    *out = k->input;
    if (cal) *cal = pid == S2_PID_JOYCON2_L ? k->cal_l : k->cal_r;
    return true;
}

void s2_link_disconnect(void) {
    disconnect_all();
}

void s2_link_start_pairing(uint32_t ms) {
    s_pair_open = true;
    s_pair_until = platform_deadline_ms(ms);
    s_sleep_quiet = false;
    LOG("s2: pairing window open for %lu s: hold Sync on the controller", (unsigned long)(ms / 1000));
    // A connected controller would keep the radio busy: let the new one in.
    // A single Joy-Con 2 stays, as the new one may be its other half (if
    // another controller turns up in pairing mode instead, the Joy-Con is
    // let go then; see s2c_on_advertisement).
    const link_t *one = links_used() == 1 ? first_used() : NULL;
    if (one && one->state == S2_LINK_READY && pid_is_joycon(one->pid)) {
        LOG("s2: keeping the %s connected: pair the other side", pid_name(one->pid));
    } else {
        disconnect_all();
    }
    update_scan();
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
    if (!links_used()) return;
    s_sleep_quiet = true;
    s_sleep_quiet_until = platform_deadline_ms(20000);
    disconnect_all();
}

void s2_link_forget(const uint8_t *addr) {
    if (!addr) {
        settings_bond_clear(&g_settings);
        settings_save_later();
        s2_link_disconnect();
        LOG("s2: all pairings forgotten");
        return;
    }
    if (!settings_bond_remove(&g_settings, addr)) return;
    settings_save_later();
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_links[i].connected && memcmp(s_links[i].peer, addr, 6) == 0) s2t_disconnect((uint8_t)i);
    }
    LOG("s2: pairing with %s forgotten", addr_str(addr));
}

void s2_link_set_paused(bool paused) {
    if (paused == s_paused) return;
    s_paused = paused;
    LOG("s2: %s", paused ? "paused" : "resumed");
    if (paused) {
        s_pause_quiet_until = platform_deadline_ms(20000);
        disconnect_all();
    }
    update_scan();
}

void s2_link_start_gyro_calibration(void) {
    if (!links_ready()) return;
    s_cal_sum[0] = s_cal_sum[1] = s_cal_sum[2] = 0;
    s_cal_n = 0;
    s_cal_active = true;
    s_cal_end = platform_deadline_ms(2000);
    LOG("s2: gyro calibration started, keep the controller still");
}
