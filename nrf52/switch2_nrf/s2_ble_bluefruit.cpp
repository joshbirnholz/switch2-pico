// Bluetooth LE transport for the Switch 2 controller on nRF52840 boards
// (Adafruit Bluefruit library on the Nordic S140 SoftDevice).
// See core/src/s2_transport.h.
//
// Bluefruit invokes its callbacks from its own FreeRTOS tasks. Every event is
// copied into a queue and handed to the shared core from s2t_task(), which
// runs in the Arduino loop, so the core stays single threaded.
//
// Unlike BTstack, Bluefruit never starts SMP pairing on its own (a Security
// Request from the peripheral is simply ignored), which is what Switch 2
// controllers need.
//
// Bluefruit's enableNotify() writes the CCCD with a Write Command (no
// response). Switch 2 controllers ignore that, so notifications never start;
// CCCDs are written here with proper Write Requests instead.

#include <Arduino.h>
#include <bluefruit.h>

#include "log.h"
#include "s2_proto.h"
#include "s2_transport.h"
#include "settings.h"

// 7.5-15 ms connection interval (units of 1.25 ms).
#define CONN_INTERVAL_MIN 6
#define CONN_INTERVAL_MAX 12
#define ATT_MTU_WANTED    247

// ---------------------------------------------------------------------------
// Event queue (Bluefruit tasks -> loop)
// ---------------------------------------------------------------------------
enum ev_type_t : uint8_t {
    EV_STACK_READY,
    EV_ADV,
    EV_LINK_UP,
    EV_CONNECT_FAILED,
    EV_GATT_READY,
    EV_CMD_RSP,
    EV_INPUT,
    EV_INPUT_ENABLED,
    EV_DISCONNECTED,
    EV_WRITE_FAILED,
    EV_POWER,
};

#define EV_DATA_MAX 128

struct ev_t {
    ev_type_t type;
    uint8_t link;
    uint8_t ok;
    uint8_t addr_type;
    int8_t rssi;
    uint16_t value;          // conn interval / reason
    uint8_t addr[6];         // big-endian
    uint16_t len;
    const char *error;       // static string
    uint8_t data[EV_DATA_MAX];
};

static QueueHandle_t s_queue;
static uint32_t s_dropped;

static void post(const ev_t &e) {
    if (xQueueSend(s_queue, &e, 0) != pdTRUE) s_dropped++;
}

static void post_simple(ev_type_t t, uint8_t link, uint8_t ok = 0, uint16_t value = 0, const char *error = nullptr) {
    ev_t e;
    memset(&e, 0, offsetof(ev_t, data));
    e.type = t;
    e.link = link;
    e.ok = ok;
    e.value = value;
    e.error = error;
    post(e);
}

static void post_data(ev_type_t t, uint8_t link, const uint8_t *data, uint16_t len) {
    ev_t e;
    memset(&e, 0, offsetof(ev_t, data));
    e.type = t;
    e.link = link;
    if (len > EV_DATA_MAX) len = EV_DATA_MAX;
    e.len = len;
    memcpy(e.data, data, len);
    post(e);
}

// ---------------------------------------------------------------------------
// GATT objects, one set per link (a Bluefruit client characteristic belongs
// to one connection). Bluefruit wants UUIDs least-significant byte first.
// ---------------------------------------------------------------------------
static uint8_t s_uuid_svc[16], s_uuid_input[16], s_uuid_cmd[16], s_uuid_rsp[16], s_uuid_vib_pro[16],
    s_uuid_vib_jl[16], s_uuid_vib_jr[16], s_uuid_vib_gc[16], s_uuid_in_pro[16], s_uuid_in_gc[16],
    s_uuid_in_jl[16], s_uuid_in_jr[16];

static void reverse_uuid(uint8_t out[16], const uint8_t in[16]) {
    for (int i = 0; i < 16; i++) out[i] = in[15 - i];
}

#define N_POWER 4   // controller-specific input reports: Pro, GameCube, Joy-Con 2 (L), (R)

struct blink_t {
    BLEClientService *svc;
    BLEClientCharacteristic *ch_input, *ch_cmd, *ch_rsp, *ch_vib_pro, *ch_vib_jl, *ch_vib_jr, *ch_vib_gc;
    BLEClientCharacteristic *ch_vib;   // whichever rumble characteristic exists
    // Controller-specific input report, read now and then for its Power Info byte.
    BLEClientCharacteristic *ch_in[N_POWER], *ch_power;
    volatile uint16_t conn;
    // One outstanding ATT Write Request per link (SoftDevice rule). Used for
    // CCCD writes from the Bluefruit task and, if the command characteristic
    // only allows "write", for commands from the loop. The buffer must stay
    // valid until the response.
    bool cmd_use_req;
    volatile bool req_pending;
    volatile uint16_t req_handle;
    volatile uint16_t req_status;
    volatile bool cccd_busy;        // a CCCD write owns the request slot
    volatile bool mtu_done;
    uint32_t req_started;
    uint8_t req_buf[256];
    uint8_t cccd_buf[2];
    // Input reports arrive every few ms; only the newest one matters, so
    // they bypass the event queue (which they would otherwise flood).
    uint8_t input_buf[EV_DATA_MAX];
    uint16_t input_len;
    volatile bool input_new;
    volatile bool power_got;
    volatile uint8_t power_info;
};

static blink_t s_l[S2T_LINKS];
static volatile int s_connect_link = -1;   // link of the connection attempt running
static bool s_ready;

static int link_of_conn(uint16_t conn) {
    if (conn == BLE_CONN_HANDLE_INVALID) return -1;
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_l[i].conn == conn) return i;
    }
    return -1;
}

static int link_of_chr(BLEClientCharacteristic *chr) {
    for (int i = 0; i < S2T_LINKS; i++) {
        blink_t &b = s_l[i];
        if (chr == b.ch_input || chr == b.ch_rsp) return i;
        for (int k = 0; k < N_POWER; k++) {
            if (chr == b.ch_in[k]) return i;
        }
    }
    return -1;
}

static uint16_t effective_mtu(BLEConnection *c) {
    // Bluefruit stores the peer's MTU without clamping it to ours.
    if (!c) return 23;
    uint16_t m = c->getMtu();
    return m > ATT_MTU_WANTED ? ATT_MTU_WANTED : m;
}

// Write a CCCD with a Write Request and wait for the response (Bluefruit task).
static bool write_cccd(blink_t &b, uint16_t conn, uint16_t handle, uint16_t value, uint16_t *status) {
    *status = 0xFFFF;
    b.cccd_busy = true;
    uint32_t t0 = millis();
    while (b.req_pending && millis() - t0 < 2000) delay(5);
    b.req_pending = false;
    b.cccd_buf[0] = (uint8_t)value;
    b.cccd_buf[1] = (uint8_t)(value >> 8);
    ble_gattc_write_params_t p;
    memset(&p, 0, sizeof p);
    p.write_op = BLE_GATT_OP_WRITE_REQ;
    p.handle = handle;
    p.len = 2;
    p.p_value = b.cccd_buf;
    b.req_handle = handle;
    b.req_status = 0xFFFF;
    b.req_pending = true;
    uint32_t err;
    t0 = millis();
    while ((err = sd_ble_gattc_write(conn, &p)) == NRF_ERROR_BUSY && millis() - t0 < 1000) delay(5);
    bool ok = false;
    if (err == NRF_SUCCESS) {
        t0 = millis();
        while (b.req_pending && millis() - t0 < 3000) delay(2);
        ok = !b.req_pending && b.req_status == BLE_GATT_STATUS_SUCCESS;
        *status = b.req_pending ? 0xFFFE : b.req_status;
    } else {
        *status = (uint16_t)(0xE000 | (err & 0xFFF));
    }
    b.req_pending = false;
    b.cccd_busy = false;
    return ok;
}

static void input_notify_cb(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len) {
    int l = link_of_chr(chr);
    if (l < 0) return;
    blink_t &b = s_l[l];
    if (len > EV_DATA_MAX) len = EV_DATA_MAX;
    taskENTER_CRITICAL();
    memcpy(b.input_buf, data, len);
    b.input_len = len;
    b.input_new = true;
    taskEXIT_CRITICAL();
}

static void power_notify_cb(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len) {
    int l = link_of_chr(chr);
    if (l < 0) return;
    blink_t &b = s_l[l];
    if (len < 2 || b.power_got) return;
    b.power_info = data[1];
    b.power_got = true;
}

static void rsp_notify_cb(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len) {
    int l = link_of_chr(chr);
    if (l >= 0) post_data(EV_CMD_RSP, (uint8_t)l, data, len);
}

// ---------------------------------------------------------------------------
// Bluefruit callbacks (Bluefruit task context)
// ---------------------------------------------------------------------------
static void scan_cb(ble_gap_evt_adv_report_t *report) {
    // Only Nintendo adverts are interesting; drop the rest here to keep the
    // queue free.
    const uint8_t *d = report->data.p_data;
    uint16_t len = report->data.len;
    bool nintendo = false;
    for (uint16_t i = 0; i + 3 < len;) {
        uint8_t l = d[i];
        if (l == 0 || i + 1 + l > len) break;
        if (d[i + 1] == 0xFF && l >= 3 && d[i + 2] == 0x53 && d[i + 3] == 0x05) nintendo = true;
        i = (uint16_t)(i + 1 + l);
    }
    if (nintendo) {
        ev_t e;
        memset(&e, 0, offsetof(ev_t, data));
        e.type = EV_ADV;
        e.addr_type = report->peer_addr.addr_type;
        e.rssi = report->rssi;
        for (int i = 0; i < 6; i++) e.addr[i] = report->peer_addr.addr[5 - i];
        e.len = len > EV_DATA_MAX ? EV_DATA_MAX : len;
        memcpy(e.data, d, e.len);
        post(e);
    }
    Bluefruit.Scanner.resume();
}

static void connect_cb(uint16_t conn) {
    int li = s_connect_link;
    s_connect_link = -1;
    if (li < 0) {
        // Not ours (cancelled meanwhile): let it go.
        BLEConnection *c = Bluefruit.Connection(conn);
        if (c) c->disconnect();
        return;
    }
    uint8_t l = (uint8_t)li;
    blink_t &b = s_l[l];
    b.conn = conn;
    BLEConnection *c = Bluefruit.Connection(conn);
    post_simple(EV_LINK_UP, l, 1, c ? c->getConnectionInterval() : 0);

    // Discovery runs here (blocking calls are fine in this task).
    // Let the MTU exchange finish first: the SoftDevice runs one GATT client
    // procedure at a time, so discovery would fail while it is pending.
    b.mtu_done = false;
    if (c && c->requestMtuExchange(ATT_MTU_WANTED)) {
        uint32_t t0 = millis();
        while (!b.mtu_done && millis() - t0 < 2000) delay(5);
    }

    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        if (attempt) delay(200);
        if (b.conn != conn) return;
        found = b.svc->discover(conn);
    }
    if (!found) {
        post_simple(EV_GATT_READY, l, 0, 0, "Switch 2 HID service not found");
        return;
    }
    BLEClientCharacteristic *chars[] = {b.ch_input, b.ch_cmd,   b.ch_rsp,   b.ch_vib_pro, b.ch_vib_jl, b.ch_vib_jr,
                                        b.ch_vib_gc, b.ch_in[0], b.ch_in[1], b.ch_in[2],   b.ch_in[3]};
    Bluefruit.Discovery.discoverCharacteristic(conn, chars, sizeof chars / sizeof chars[0]);
    if (!b.ch_input->discovered() || !b.ch_cmd->discovered() || !b.ch_rsp->discovered()) {
        post_simple(EV_GATT_READY, l, 0, 0, "required characteristics missing");
        return;
    }
    b.ch_vib = b.ch_vib_pro->discovered() ? b.ch_vib_pro
             : b.ch_vib_jl->discovered()  ? b.ch_vib_jl
             : b.ch_vib_jr->discovered()  ? b.ch_vib_jr
             : b.ch_vib_gc->discovered()  ? b.ch_vib_gc
                                          : nullptr;
    b.ch_power = nullptr;
    for (int k = 0; k < N_POWER && !b.ch_power; k++) {
        if (b.ch_in[k]->discovered()) b.ch_power = b.ch_in[k];
    }
    b.cmd_use_req = (b.ch_cmd->properties() & 0x08) != 0;   // "write" property
    // The CCCD directly follows the value attribute on these controllers.
    uint16_t st;
    if (!write_cccd(b, conn, b.ch_rsp->valueHandle() + 1, 0x0001, &st)) {
        post_simple(EV_WRITE_FAILED, l, 1, st);
        post_simple(EV_GATT_READY, l, 0, 0, "could not enable command responses");
        return;
    }
    {
        // Report what was found so logs show handles and properties.
        ev_t e;
        memset(&e, 0, offsetof(ev_t, data));
        e.type = EV_GATT_READY;
        e.link = l;
        e.ok = 1;
        uint16_t h[5] = {b.ch_input->valueHandle(), b.ch_cmd->valueHandle(), b.ch_rsp->valueHandle(),
                         b.ch_vib ? b.ch_vib->valueHandle() : (uint16_t)0,
                         b.ch_power ? b.ch_power->valueHandle() : (uint16_t)0};
        memcpy(e.data, h, sizeof h);
        e.data[10] = b.ch_cmd->properties();
        e.data[11] = b.ch_rsp->properties();
        e.data[12] = b.ch_vib ? b.ch_vib->properties() : 0;
        e.data[13] = (uint8_t)effective_mtu(c);
        e.len = 14;
        post(e);
    }
}

static void ble_event_cb(ble_evt_t *evt) {
    switch (evt->header.evt_id) {
    case BLE_GATTC_EVT_EXCHANGE_MTU_RSP: {
        int l = link_of_conn(evt->evt.gattc_evt.conn_handle);
        if (l >= 0) s_l[l].mtu_done = true;
        break;
    }
    case BLE_GATTC_EVT_WRITE_RSP: {
        int l = link_of_conn(evt->evt.gattc_evt.conn_handle);
        if (l < 0) break;
        blink_t &b = s_l[l];
        const ble_gattc_evt_write_rsp_t &w = evt->evt.gattc_evt.params.write_rsp;
        if (w.write_op != BLE_GATT_OP_WRITE_REQ || !b.req_pending || w.handle != b.req_handle) break;
        b.req_status = evt->evt.gattc_evt.gatt_status;
        b.req_pending = false;
        if (!b.cccd_busy && b.req_status != BLE_GATT_STATUS_SUCCESS) post_simple(EV_WRITE_FAILED, (uint8_t)l, 0, b.req_status);
        break;
    }
    default:
        break;
    }
}

static void disconnect_cb(uint16_t conn, uint8_t reason) {
    int l = link_of_conn(conn);
    if (l < 0) return;
    blink_t &b = s_l[l];
    b.req_pending = false;
    b.conn = BLE_CONN_HANDLE_INVALID;
    b.ch_vib = nullptr;
    b.ch_power = nullptr;
    b.input_new = false;
    post_simple(EV_DISCONNECTED, (uint8_t)l, 0, reason);
}

// Workers run in Bluefruit's callback task; the link travels as the argument.
// Subscribe to the controller-specific report, take one, unsubscribe.
static void power_worker(uint32_t arg) {
    uint8_t l = (uint8_t)arg;
    blink_t &b = s_l[l];
    uint16_t conn = b.conn, st = 0;
    BLEClientCharacteristic *ch = b.ch_power;
    if (conn == BLE_CONN_HANDLE_INVALID || !ch) {
        post_simple(EV_POWER, l, 0);
        return;
    }
    uint16_t cccd = ch->valueHandle() + 1;
    b.power_got = false;
    bool ok = write_cccd(b, conn, cccd, 0x0001, &st);
    if (ok) {
        uint32_t t0 = millis();
        while (!b.power_got && millis() - t0 < 1000 && b.conn == conn) delay(2);
    }
    if (b.conn == conn) write_cccd(b, conn, cccd, 0x0000, &st);
    post_simple(EV_POWER, l, ok && b.power_got, b.power_info);
}

static void enable_input_worker(uint32_t arg) {
    uint8_t l = (uint8_t)arg;
    blink_t &b = s_l[l];
    uint16_t conn = b.conn, st = 0;
    bool ok = conn != BLE_CONN_HANDLE_INVALID && write_cccd(b, conn, b.ch_input->valueHandle() + 1, 0x0001, &st);
    if (!ok && conn != BLE_CONN_HANDLE_INVALID) post_simple(EV_WRITE_FAILED, l, 2, st);
    post_simple(EV_INPUT_ENABLED, l, ok);
}

// ---------------------------------------------------------------------------
// s2_transport.h
// ---------------------------------------------------------------------------
extern "C" {

void s2t_init(void) {
    s_queue = xQueueCreate(32, sizeof(ev_t));

    reverse_uuid(s_uuid_svc, S2_UUID_HID_SERVICE);
    reverse_uuid(s_uuid_input, S2_UUID_INPUT_COMMON);
    reverse_uuid(s_uuid_cmd, S2_UUID_CMD_WRITE);
    reverse_uuid(s_uuid_rsp, S2_UUID_CMD_RESPONSE);
    reverse_uuid(s_uuid_vib_pro, S2_UUID_VIB_PRO);
    reverse_uuid(s_uuid_vib_jl, S2_UUID_VIB_JOYCON_L);
    reverse_uuid(s_uuid_vib_jr, S2_UUID_VIB_JOYCON_R);
    reverse_uuid(s_uuid_vib_gc, S2_UUID_VIB_GC);
    reverse_uuid(s_uuid_in_pro, S2_UUID_INPUT_PRO);
    reverse_uuid(s_uuid_in_gc, S2_UUID_INPUT_GC);
    reverse_uuid(s_uuid_in_jl, S2_UUID_INPUT_JOYCON_L);
    reverse_uuid(s_uuid_in_jr, S2_UUID_INPUT_JOYCON_R);
    const uint8_t *power_uuids[N_POWER] = {s_uuid_in_pro, s_uuid_in_gc, s_uuid_in_jl, s_uuid_in_jr};
    for (int i = 0; i < S2T_LINKS; i++) {
        blink_t &b = s_l[i];
        b.conn = BLE_CONN_HANDLE_INVALID;
        b.svc = new BLEClientService(BLEUuid(s_uuid_svc));
        b.ch_input = new BLEClientCharacteristic(BLEUuid(s_uuid_input));
        b.ch_cmd = new BLEClientCharacteristic(BLEUuid(s_uuid_cmd));
        b.ch_rsp = new BLEClientCharacteristic(BLEUuid(s_uuid_rsp));
        b.ch_vib_pro = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_pro));
        b.ch_vib_jl = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_jl));
        b.ch_vib_jr = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_jr));
        b.ch_vib_gc = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_gc));
        for (int k = 0; k < N_POWER; k++) b.ch_in[k] = new BLEClientCharacteristic(BLEUuid(power_uuids[k]));
    }

    // Large MTU and event length for the connections to the controller(s):
    // two at once for a Joy-Con 2 (L) and (R).
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin(0, S2T_LINKS);
    Bluefruit.autoConnLed(false);
    // Applied to each new connection. Lower settings cut the radio's peak
    // current, for boards on a weak USB supply.
    static const int8_t TX_POWER[] = {8, 4, 0, -4};
    Bluefruit.setTxPower(TX_POWER[g_settings.ble_tx_power < 4 ? g_settings.ble_tx_power : 0]);
    Bluefruit.setName("Switch2-Pico");

    // Use the chip's address as a *public* address. A paired controller only
    // accepts reconnections from its host's address as a public address (the
    // console's type); nRF chips default to a random static one, which the
    // controller ignores ("connection failed to be established", 0x3e).
    // The six bytes stay the same, so existing pairings still match.
    ble_gap_addr_t addr = Bluefruit.getAddr();
    addr.addr_id_peer = 0;
    addr.addr_type = BLE_GAP_ADDR_TYPE_PUBLIC;
    if (!Bluefruit.setAddr(&addr)) LOG("ble: could not switch to a public address");

    for (int i = 0; i < S2T_LINKS; i++) {
        blink_t &b = s_l[i];
        b.svc->begin();
        b.ch_input->setNotifyCallback(input_notify_cb, false);
        b.ch_input->begin(b.svc);
        b.ch_cmd->begin(b.svc);
        b.ch_rsp->setNotifyCallback(rsp_notify_cb, false);
        b.ch_rsp->begin(b.svc);
        b.ch_vib_pro->begin(b.svc);
        b.ch_vib_jl->begin(b.svc);
        b.ch_vib_jr->begin(b.svc);
        b.ch_vib_gc->begin(b.svc);
        for (int k = 0; k < N_POWER; k++) {
            b.ch_in[k]->setNotifyCallback(power_notify_cb, false);
            b.ch_in[k]->begin(b.svc);
        }
    }

    Bluefruit.setEventCallback(ble_event_cb);
    Bluefruit.Central.setConnectCallback(connect_cb);
    Bluefruit.Central.setDisconnectCallback(disconnect_cb);
    Bluefruit.Central.setConnInterval(CONN_INTERVAL_MIN, CONN_INTERVAL_MAX);

    Bluefruit.Scanner.setRxCallback(scan_cb);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterMSD(S2_NINTENDO_COMPANY_ID);

    s_ready = true;
    post_simple(EV_STACK_READY, 0);
}

static void deliver_input(void) {
    for (int l = 0; l < S2T_LINKS; l++) {
        blink_t &b = s_l[l];
        if (!b.input_new) continue;
        uint8_t buf[EV_DATA_MAX];
        uint16_t len;
        taskENTER_CRITICAL();
        memcpy(buf, b.input_buf, b.input_len);
        len = b.input_len;
        b.input_new = false;
        taskEXIT_CRITICAL();
        if (b.conn != BLE_CONN_HANDLE_INVALID) s2c_on_input_report((uint8_t)l, buf, len);
    }
}

void s2t_task(void) {
    ev_t e;
    deliver_input();
    while (xQueueReceive(s_queue, &e, 0) == pdTRUE) {
        uint8_t l = e.link;
        switch (e.type) {
        case EV_STACK_READY: s2c_on_stack_ready(); break;
        case EV_ADV: s2c_on_advertisement(e.addr, e.addr_type, e.rssi, e.data, e.len); break;
        case EV_LINK_UP: s2c_on_link_up(l, e.value); break;
        case EV_CONNECT_FAILED: s2c_on_connect_failed(l, (uint8_t)e.value); break;
        case EV_GATT_READY:
            if (e.ok && e.len >= 14) {
                uint16_t h[5];
                memcpy(h, e.data, sizeof h);
                LOG("s2: link %u handles input=%04x cmd=%04x rsp=%04x vib=%04x power=%04x, props cmd=%02x rsp=%02x vib=%02x, mtu %u",
                    l, h[0], h[1], h[2], h[3], h[4], e.data[10], e.data[11], e.data[12], e.data[13]);
                LOG("s2: link %u commands use %s", l, s_l[l].cmd_use_req ? "write requests" : "write commands");
            }
            s2c_on_gatt_ready(l, e.ok, e.error);
            break;
        case EV_WRITE_FAILED:
            if (e.ok) LOG("s2: link %u enabling %s notifications failed (status 0x%04x)", l, e.ok == 1 ? "command response" : "input", e.value);
            else LOG("s2: link %u command write rejected, ATT status 0x%04x", l, e.value);
            break;
        case EV_CMD_RSP: s2c_on_command_response(l, e.data, e.len); break;
        case EV_INPUT: break;
        case EV_INPUT_ENABLED: s2c_on_input_enabled(l, e.ok); break;
        case EV_POWER: s2c_on_power_info(l, e.ok, (uint8_t)e.value); break;
        case EV_DISCONNECTED: s2c_on_disconnected(l, (uint8_t)e.value); break;
        }
    }
    if (s_dropped) {
        LOG("ble: %lu events dropped (queue full)", (unsigned long)s_dropped);
        s_dropped = 0;
    }
}

bool s2t_ready(void) {
    return s_ready;
}

void s2t_start_scan(bool low_duty) {
    Bluefruit.Scanner.stop();
    // Units of 0.625 ms: 30 ms window every 60 ms (low duty: every 320 ms).
    if (low_duty) Bluefruit.Scanner.setInterval(512, 48);
    else Bluefruit.Scanner.setInterval(96, 48);
    Bluefruit.Scanner.start(0);
}

void s2t_stop_scan(void) {
    Bluefruit.Scanner.stop();
}

bool s2t_connect(uint8_t l, const uint8_t addr[6], uint8_t addr_type) {
    if (l >= S2T_LINKS || s_connect_link >= 0) return false;
    ble_gap_addr_t a;
    memset(&a, 0, sizeof a);
    a.addr_type = addr_type;
    for (int i = 0; i < 6; i++) a.addr[i] = addr[5 - i];
    Bluefruit.Scanner.stop();
    s_connect_link = l;
    if (!Bluefruit.Central.connect(&a)) {
        s_connect_link = -1;
        return false;
    }
    return true;
}

void s2t_cancel_connect(void) {
    if (s_connect_link >= 0) {
        sd_ble_gap_connect_cancel();
        s_connect_link = -1;
    }
}

void s2t_disconnect(uint8_t l) {
    if (l >= S2T_LINKS) return;
    uint16_t conn = s_l[l].conn;
    if (conn != BLE_CONN_HANDLE_INVALID) {
        BLEConnection *c = Bluefruit.Connection(conn);
        if (c) c->disconnect();
    }
}

bool s2t_has_char(uint8_t l, s2t_char_t ch) {
    if (l >= S2T_LINKS) return false;
    if (ch == S2T_CHAR_COMMAND) return s_l[l].ch_cmd && s_l[l].ch_cmd->discovered();
    return s_l[l].ch_vib != nullptr;
}

s2t_write_result_t s2t_write(uint8_t l, s2t_char_t ch, const uint8_t *data, uint16_t len) {
    if (l >= S2T_LINKS) return S2T_WRITE_ERROR;
    blink_t &b = s_l[l];
    uint16_t conn = b.conn;
    if (conn == BLE_CONN_HANDLE_INVALID || !s2t_has_char(l, ch)) return S2T_WRITE_ERROR;
    BLEClientCharacteristic *chr = ch == S2T_CHAR_COMMAND ? b.ch_cmd : b.ch_vib;
    BLEConnection *c = Bluefruit.Connection(conn);
    if (!c) return S2T_WRITE_ERROR;
    if (len + 3u > effective_mtu(c)) return S2T_WRITE_ERROR;
    if (ch == S2T_CHAR_COMMAND && b.cmd_use_req) {
        if (b.cccd_busy) return S2T_WRITE_BUSY;
        if (b.req_pending) {
            // A lost response must not wedge the command channel for good.
            if (millis() - b.req_started < 2000) return S2T_WRITE_BUSY;
            b.req_pending = false;
        }
        if (len > sizeof b.req_buf) return S2T_WRITE_ERROR;
        memcpy(b.req_buf, data, len);
        ble_gattc_write_params_t p;
        memset(&p, 0, sizeof p);
        p.write_op = BLE_GATT_OP_WRITE_REQ;
        p.handle = chr->valueHandle();
        p.len = len;
        p.p_value = b.req_buf;
        b.req_handle = p.handle;
        b.req_pending = true;
        b.req_started = millis();
        uint32_t err = sd_ble_gattc_write(conn, &p);
        if (err == NRF_SUCCESS) return S2T_WRITE_OK;
        b.req_pending = false;
        if (err == NRF_ERROR_BUSY || err == NRF_ERROR_RESOURCES) return S2T_WRITE_BUSY;
        return S2T_WRITE_ERROR;
    }
    // Write without response, straight to the SoftDevice so a full TX queue
    // never blocks the loop (Bluefruit's write() would wait for a buffer).
    ble_gattc_write_params_t p;
    memset(&p, 0, sizeof p);
    p.write_op = BLE_GATT_OP_WRITE_CMD;
    p.handle = chr->valueHandle();
    p.len = len;
    p.p_value = data;
    uint32_t err = sd_ble_gattc_write(conn, &p);
    if (err == NRF_SUCCESS) return S2T_WRITE_OK;
    if (err == NRF_ERROR_RESOURCES || err == NRF_ERROR_BUSY) return S2T_WRITE_BUSY;
    return S2T_WRITE_ERROR;
}

void s2t_enable_input(uint8_t l) {
    if (l >= S2T_LINKS) return;
    // The CCCD write blocks until the controller answers; run it in
    // Bluefruit's callback task instead of the loop.
    ada_callback(NULL, 0, enable_input_worker, l);
}

bool s2t_poll_power(uint8_t l) {
    if (l >= S2T_LINKS) return false;
    blink_t &b = s_l[l];
    if (b.conn == BLE_CONN_HANDLE_INVALID || !b.ch_power || b.cccd_busy) return false;
    ada_callback(NULL, 0, power_worker, l);
    return true;
}

void s2t_local_address(uint8_t out[6]) {
    uint8_t mac[6];
    Bluefruit.getAddr(mac);
    for (int i = 0; i < 6; i++) out[i] = mac[5 - i];
}

uint16_t s2t_mtu(uint8_t l) {
    if (l >= S2T_LINKS) return 23;
    uint16_t conn = s_l[l].conn;
    BLEConnection *c = conn != BLE_CONN_HANDLE_INVALID ? Bluefruit.Connection(conn) : nullptr;
    return effective_mtu(c);
}

}  // extern "C"
