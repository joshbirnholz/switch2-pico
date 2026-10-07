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

#include <Arduino.h>
#include <bluefruit.h>

#include "log.h"
#include "s2_proto.h"
#include "s2_transport.h"

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
};

#define EV_DATA_MAX 128

struct ev_t {
    ev_type_t type;
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

static void post_simple(ev_type_t t, uint8_t ok = 0, uint16_t value = 0, const char *error = nullptr) {
    ev_t e;
    memset(&e, 0, offsetof(ev_t, data));
    e.type = t;
    e.ok = ok;
    e.value = value;
    e.error = error;
    post(e);
}

static void post_data(ev_type_t t, const uint8_t *data, uint16_t len) {
    ev_t e;
    memset(&e, 0, offsetof(ev_t, data));
    e.type = t;
    if (len > EV_DATA_MAX) len = EV_DATA_MAX;
    e.len = len;
    memcpy(e.data, data, len);
    post(e);
}

// ---------------------------------------------------------------------------
// GATT objects. Bluefruit wants UUIDs least-significant byte first.
// ---------------------------------------------------------------------------
static uint8_t s_uuid_svc[16], s_uuid_input[16], s_uuid_cmd[16], s_uuid_rsp[16], s_uuid_vib_pro[16],
    s_uuid_vib_jl[16], s_uuid_vib_jr[16];

static void reverse_uuid(uint8_t out[16], const uint8_t in[16]) {
    for (int i = 0; i < 16; i++) out[i] = in[15 - i];
}

static BLEClientService *s_svc;
static BLEClientCharacteristic *s_ch_input, *s_ch_cmd, *s_ch_rsp, *s_ch_vib_pro, *s_ch_vib_jl, *s_ch_vib_jr;
static BLEClientCharacteristic *s_ch_vib;   // whichever rumble characteristic exists

// Commands go out as ATT Write Requests when the characteristic allows it
// (that's what the controller answers; Write Commands are silently ignored).
// A Write Request keeps its buffer until the response, one at a time.
static bool s_cmd_use_req;
static volatile bool s_req_pending;
static volatile bool s_cccd_busy;        // enable_input_worker owns the request slot
static uint32_t s_req_started;
static uint8_t s_req_buf[256];

static volatile uint16_t s_conn = BLE_CONN_HANDLE_INVALID;
static volatile bool s_connecting;
static bool s_ready;

static void input_notify_cb(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len) {
    (void)chr;
    post_data(EV_INPUT, data, len);
}

static void rsp_notify_cb(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len) {
    (void)chr;
    post_data(EV_CMD_RSP, data, len);
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
    s_conn = conn;
    s_connecting = false;
    BLEConnection *c = Bluefruit.Connection(conn);
    post_simple(EV_LINK_UP, 1, c ? c->getConnectionInterval() : 0);

    // Discovery runs here (blocking calls are fine in this task).
    if (c) c->requestMtuExchange(ATT_MTU_WANTED);
    uint32_t t0 = millis();
    while (c && c->getMtu() <= 23 && millis() - t0 < 1000) delay(10);

    if (!s_svc->discover(conn)) {
        post_simple(EV_GATT_READY, 0, 0, "Switch 2 HID service not found");
        return;
    }
    Bluefruit.Discovery.discoverCharacteristic(conn, *s_ch_input, *s_ch_cmd, *s_ch_rsp, *s_ch_vib_pro, *s_ch_vib_jl,
                                               *s_ch_vib_jr);
    if (!s_ch_input->discovered() || !s_ch_cmd->discovered() || !s_ch_rsp->discovered()) {
        post_simple(EV_GATT_READY, 0, 0, "required characteristics missing");
        return;
    }
    s_ch_vib = s_ch_vib_pro->discovered() ? s_ch_vib_pro
             : s_ch_vib_jl->discovered()  ? s_ch_vib_jl
             : s_ch_vib_jr->discovered()  ? s_ch_vib_jr
                                          : nullptr;
    s_cmd_use_req = (s_ch_cmd->properties() & 0x08) != 0;   // "write" property
    if (!s_ch_rsp->enableNotify()) {
        post_simple(EV_GATT_READY, 0, 0, "could not enable command responses");
        return;
    }
    {
        // Report what was found so logs show handles and properties.
        ev_t e;
        memset(&e, 0, offsetof(ev_t, data));
        e.type = EV_GATT_READY;
        e.ok = 1;
        uint16_t h[4] = {s_ch_input->valueHandle(), s_ch_cmd->valueHandle(), s_ch_rsp->valueHandle(),
                         s_ch_vib ? s_ch_vib->valueHandle() : (uint16_t)0};
        memcpy(e.data, h, sizeof h);
        e.data[8] = s_ch_cmd->properties();
        e.data[9] = s_ch_rsp->properties();
        e.data[10] = s_ch_vib ? s_ch_vib->properties() : 0;
        e.data[11] = (uint8_t)(c ? c->getMtu() : 23);
        e.len = 12;
        post(e);
    }
}

static void ble_event_cb(ble_evt_t *evt) {
    if (evt->header.evt_id != BLE_GATTC_EVT_WRITE_RSP) return;
    const ble_gattc_evt_write_rsp_t &w = evt->evt.gattc_evt.params.write_rsp;
    if (w.write_op != BLE_GATT_OP_WRITE_REQ || !s_req_pending) return;
    if (s_ch_cmd && w.handle != s_ch_cmd->valueHandle()) return;
    s_req_pending = false;
    if (evt->evt.gattc_evt.gatt_status != BLE_GATT_STATUS_SUCCESS)
        post_simple(EV_WRITE_FAILED, 0, evt->evt.gattc_evt.gatt_status);
}

static void disconnect_cb(uint16_t conn, uint8_t reason) {
    (void)conn;
    s_req_pending = false;
    s_conn = BLE_CONN_HANDLE_INVALID;
    s_connecting = false;
    s_ch_vib = nullptr;
    post_simple(EV_DISCONNECTED, 0, reason);
}

static void enable_input_worker(void) {
    // The SoftDevice runs one Write Request at a time per link.
    s_cccd_busy = true;
    uint32_t t0 = millis();
    while (s_req_pending && millis() - t0 < 2000) delay(5);
    bool ok = s_conn != BLE_CONN_HANDLE_INVALID && s_ch_input->enableNotify();
    s_cccd_busy = false;
    post_simple(EV_INPUT_ENABLED, ok);
}

// ---------------------------------------------------------------------------
// s2_transport.h
// ---------------------------------------------------------------------------
extern "C" {

void s2t_init(void) {
    s_queue = xQueueCreate(24, sizeof(ev_t));

    reverse_uuid(s_uuid_svc, S2_UUID_HID_SERVICE);
    reverse_uuid(s_uuid_input, S2_UUID_INPUT_COMMON);
    reverse_uuid(s_uuid_cmd, S2_UUID_CMD_WRITE);
    reverse_uuid(s_uuid_rsp, S2_UUID_CMD_RESPONSE);
    reverse_uuid(s_uuid_vib_pro, S2_UUID_VIB_PRO);
    reverse_uuid(s_uuid_vib_jl, S2_UUID_VIB_JOYCON_L);
    reverse_uuid(s_uuid_vib_jr, S2_UUID_VIB_JOYCON_R);
    s_svc = new BLEClientService(BLEUuid(s_uuid_svc));
    s_ch_input = new BLEClientCharacteristic(BLEUuid(s_uuid_input));
    s_ch_cmd = new BLEClientCharacteristic(BLEUuid(s_uuid_cmd));
    s_ch_rsp = new BLEClientCharacteristic(BLEUuid(s_uuid_rsp));
    s_ch_vib_pro = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_pro));
    s_ch_vib_jl = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_jl));
    s_ch_vib_jr = new BLEClientCharacteristic(BLEUuid(s_uuid_vib_jr));

    // Large MTU and event length for the connection to the controller.
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin(0, 1);
    Bluefruit.autoConnLed(false);
    Bluefruit.setTxPower(8);
    Bluefruit.setName("Switch2-Pico");

    s_svc->begin();
    s_ch_input->setNotifyCallback(input_notify_cb, false);
    s_ch_input->begin(s_svc);
    s_ch_cmd->begin(s_svc);
    s_ch_rsp->setNotifyCallback(rsp_notify_cb, false);
    s_ch_rsp->begin(s_svc);
    s_ch_vib_pro->begin(s_svc);
    s_ch_vib_jl->begin(s_svc);
    s_ch_vib_jr->begin(s_svc);

    Bluefruit.setEventCallback(ble_event_cb);
    Bluefruit.Central.setConnectCallback(connect_cb);
    Bluefruit.Central.setDisconnectCallback(disconnect_cb);
    Bluefruit.Central.setConnInterval(CONN_INTERVAL_MIN, CONN_INTERVAL_MAX);

    Bluefruit.Scanner.setRxCallback(scan_cb);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterMSD(S2_NINTENDO_COMPANY_ID);

    s_ready = true;
    post_simple(EV_STACK_READY);
}

void s2t_task(void) {
    ev_t e;
    while (xQueueReceive(s_queue, &e, 0) == pdTRUE) {
        switch (e.type) {
        case EV_STACK_READY: s2c_on_stack_ready(); break;
        case EV_ADV: s2c_on_advertisement(e.addr, e.addr_type, e.rssi, e.data, e.len); break;
        case EV_LINK_UP: s2c_on_link_up(e.value); break;
        case EV_CONNECT_FAILED: s2c_on_connect_failed((uint8_t)e.value); break;
        case EV_GATT_READY:
            if (e.ok && e.len >= 12) {
                uint16_t h[4];
                memcpy(h, e.data, sizeof h);
                LOG("s2: handles input=%04x cmd=%04x rsp=%04x vib=%04x, props cmd=%02x rsp=%02x vib=%02x, mtu %u",
                    h[0], h[1], h[2], h[3], e.data[8], e.data[9], e.data[10], e.data[11]);
                LOG("s2: commands use %s", s_cmd_use_req ? "write requests" : "write commands");
            }
            s2c_on_gatt_ready(e.ok, e.error);
            break;
        case EV_WRITE_FAILED: LOG("s2: command write rejected, ATT status 0x%04x", e.value); break;
        case EV_CMD_RSP: s2c_on_command_response(e.data, e.len); break;
        case EV_INPUT: s2c_on_input_report(e.data, e.len); break;
        case EV_INPUT_ENABLED: s2c_on_input_enabled(e.ok); break;
        case EV_DISCONNECTED: s2c_on_disconnected((uint8_t)e.value); break;
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

bool s2t_connect(const uint8_t addr[6], uint8_t addr_type) {
    ble_gap_addr_t a;
    memset(&a, 0, sizeof a);
    a.addr_type = addr_type;
    for (int i = 0; i < 6; i++) a.addr[i] = addr[5 - i];
    Bluefruit.Scanner.stop();
    s_connecting = true;
    if (!Bluefruit.Central.connect(&a)) {
        s_connecting = false;
        return false;
    }
    return true;
}

void s2t_cancel_connect(void) {
    if (s_connecting) {
        sd_ble_gap_connect_cancel();
        s_connecting = false;
    }
}

void s2t_disconnect(void) {
    uint16_t conn = s_conn;
    if (conn != BLE_CONN_HANDLE_INVALID) {
        BLEConnection *c = Bluefruit.Connection(conn);
        if (c) c->disconnect();
    }
}

bool s2t_has_char(s2t_char_t ch) {
    if (ch == S2T_CHAR_COMMAND) return s_ch_cmd && s_ch_cmd->discovered();
    return s_ch_vib != nullptr;
}

s2t_write_result_t s2t_write(s2t_char_t ch, const uint8_t *data, uint16_t len) {
    uint16_t conn = s_conn;
    if (conn == BLE_CONN_HANDLE_INVALID || !s2t_has_char(ch)) return S2T_WRITE_ERROR;
    BLEClientCharacteristic *chr = ch == S2T_CHAR_COMMAND ? s_ch_cmd : s_ch_vib;
    BLEConnection *c = Bluefruit.Connection(conn);
    if (!c) return S2T_WRITE_ERROR;
    if (len + 3u > c->getMtu()) return S2T_WRITE_ERROR;
    if (ch == S2T_CHAR_COMMAND && s_cmd_use_req) {
        if (s_cccd_busy) return S2T_WRITE_BUSY;
        if (s_req_pending) {
            // A lost response must not wedge the command channel for good.
            if (millis() - s_req_started < 2000) return S2T_WRITE_BUSY;
            s_req_pending = false;
        }
        if (len > sizeof s_req_buf) return S2T_WRITE_ERROR;
        memcpy(s_req_buf, data, len);
        ble_gattc_write_params_t p;
        memset(&p, 0, sizeof p);
        p.write_op = BLE_GATT_OP_WRITE_REQ;
        p.handle = chr->valueHandle();
        p.len = len;
        p.p_value = s_req_buf;
        s_req_pending = true;
        s_req_started = millis();
        uint32_t err = sd_ble_gattc_write(conn, &p);
        if (err == NRF_SUCCESS) return S2T_WRITE_OK;
        s_req_pending = false;
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

void s2t_enable_input(void) {
    // enableNotify() blocks until the controller answers; run it in
    // Bluefruit's callback task instead of the loop.
    ada_callback(NULL, 0, enable_input_worker);
}

void s2t_local_address(uint8_t out[6]) {
    uint8_t mac[6];
    Bluefruit.getAddr(mac);
    for (int i = 0; i < 6; i++) out[i] = mac[5 - i];
}

uint16_t s2t_mtu(void) {
    uint16_t conn = s_conn;
    BLEConnection *c = conn != BLE_CONN_HANDLE_INVALID ? Bluefruit.Connection(conn) : nullptr;
    return c ? c->getMtu() : 23;
}

}  // extern "C"
