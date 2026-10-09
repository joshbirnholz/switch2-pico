// Bluetooth LE transport for the Switch 2 controller on the Pico W / Pico 2 W
// (BTstack on the CYW43439). See core/src/s2_transport.h.

#include <string.h>

#include "btstack.h"

#include "log.h"
#include "platform.h"
#include "s2_proto.h"
#include "s2_transport.h"

// Connection parameters: 7.5-15 ms interval, no latency, 2 s supervision.
#define CONN_INTERVAL_MIN 6
#define CONN_INTERVAL_MAX 12
#define CONN_SUPERVISION  200

typedef enum {
    GP_NONE,
    GP_SERVICES,
    GP_CHARACTERISTICS,
    GP_CMD_CCCD,
    GP_IDLE,
    GP_INPUT_CCCD,
    GP_POWER_ON,      // subscribing to the controller-specific report
    GP_POWER_WAIT,    // ... waiting for one
    GP_POWER_OFF,     // ... unsubscribing
} gatt_phase_t;

static btstack_packet_callback_registration_t s_hci_cb;
static int s_connecting = -1;      // link of the connection attempt running

// Per link (see s2_transport.h): its connection and what GATT discovery found.
typedef struct {
    hci_con_handle_t con;
    gatt_phase_t phase;
    gatt_client_service_t service;
    bool have_service;
    gatt_client_characteristic_t ch_input, ch_cmd, ch_cmd_rsp, ch_vib;
    bool have_input, have_cmd, have_cmd_rsp, have_vib;
    gatt_client_notification_t notif_input, notif_cmd;
    // Controller-specific input report, read now and then for its Power Info byte.
    gatt_client_characteristic_t ch_power;
    bool have_power, power_got;
    uint8_t power_info;
    uint32_t power_deadline;
    gatt_client_notification_t notif_power;
    // Commands go out as ATT Write Requests when the characteristic allows
    // it (the controller ignores Write Commands there). BTstack runs one
    // request at a time per connection and needs the buffer until it completes.
    bool cmd_use_req;
    bool req_pending;
    bool input_cccd_wanted;   // enable input once the pending write completes
    uint8_t req_buf[256];
} blink_t;

static blink_t s_links[S2T_LINKS];

static void start_input_cccd(uint8_t l);
static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static bool uuid_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

static int link_of(hci_con_handle_t con) {
    if (con == HCI_CON_HANDLE_INVALID) return -1;
    for (int i = 0; i < S2T_LINKS; i++) {
        if (s_links[i].con == con) return i;
    }
    return -1;
}

static void reset_gatt(uint8_t l) {
    blink_t *b = &s_links[l];
    if (b->con != HCI_CON_HANDLE_INVALID) {
        gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_input);
        gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_cmd);
        gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_power);
    }
    b->con = HCI_CON_HANDLE_INVALID;
    b->phase = GP_NONE;
    b->req_pending = false;
    b->input_cccd_wanted = false;
    b->have_service = b->have_input = b->have_cmd = b->have_cmd_rsp = b->have_vib = b->have_power = false;
}

// Enable notifications by writing the CCCD with a Write Request at
// value_handle + 1, where these controllers put it (as the nRF52840 build
// does). BTstack's gatt_client_write_client_characteristic_configuration()
// first looks the CCCD up with a Read By Type request, which the controller
// never answers, so the write never happened.
static uint8_t s_cccd_value[2] = {0x01, 0x00};   // notifications on
static uint8_t s_cccd_off[2] = {0x00, 0x00};

static uint8_t write_cccd(blink_t *b, gatt_client_characteristic_t *c) {
    return gatt_client_write_value_of_characteristic(gatt_handler, b->con, (uint16_t)(c->value_handle + 1),
                                                     sizeof s_cccd_value, s_cccd_value);
}

static void power_unsubscribe(uint8_t l) {
    blink_t *b = &s_links[l];
    b->phase = GP_POWER_OFF;
    if (gatt_client_write_value_of_characteristic(gatt_handler, b->con, (uint16_t)(b->ch_power.value_handle + 1),
                                                  sizeof s_cccd_off, s_cccd_off) != ERROR_CODE_SUCCESS) {
        b->phase = GP_IDLE;
        gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_power);
        s2c_on_power_info(l, b->power_got, b->power_info);
    }
}

static void gatt_fail(uint8_t l, const char *why) {
    s_links[l].phase = GP_IDLE;
    s2c_on_gatt_ready(l, false, why);
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t ev = hci_event_packet_get_type(packet);

    if (ev == GATT_EVENT_NOTIFICATION) {
        int l = link_of(gatt_event_notification_get_handle(packet));
        if (l < 0) return;
        blink_t *b = &s_links[l];
        uint16_t h = gatt_event_notification_get_value_handle(packet);
        const uint8_t *v = gatt_event_notification_get_value(packet);
        uint16_t len = gatt_event_notification_get_value_length(packet);
        if (b->have_input && h == b->ch_input.value_handle) s2c_on_input_report((uint8_t)l, v, len);
        else if (b->have_power && h == b->ch_power.value_handle) {
            if (b->phase == GP_POWER_WAIT && len >= 2 && !b->power_got) {
                b->power_got = true;
                b->power_info = v[1];
                power_unsubscribe((uint8_t)l);
            }
        }
        else if (b->have_cmd_rsp && h == b->ch_cmd_rsp.value_handle) s2c_on_command_response((uint8_t)l, v, len);
        return;
    }

    switch (ev) {
    case GATT_EVENT_SERVICE_QUERY_RESULT: {
        int l = link_of(gatt_event_service_query_result_get_handle(packet));
        if (l < 0) break;
        gatt_event_service_query_result_get_service(packet, &s_links[l].service);
        s_links[l].have_service = true;
        break;
    }
    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        int l = link_of(gatt_event_characteristic_query_result_get_handle(packet));
        if (l < 0) break;
        blink_t *b = &s_links[l];
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        if (uuid_eq(c.uuid128, S2_UUID_INPUT_COMMON)) { b->ch_input = c; b->have_input = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_CMD_WRITE)) { b->ch_cmd = c; b->have_cmd = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_CMD_RESPONSE)) { b->ch_cmd_rsp = c; b->have_cmd_rsp = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_VIB_PRO) || uuid_eq(c.uuid128, S2_UUID_VIB_JOYCON_L) ||
                 uuid_eq(c.uuid128, S2_UUID_VIB_JOYCON_R) || uuid_eq(c.uuid128, S2_UUID_VIB_GC)) { b->ch_vib = c; b->have_vib = true; }
        else if (uuid_eq(c.uuid128, S2_UUID_INPUT_PRO) || uuid_eq(c.uuid128, S2_UUID_INPUT_GC) ||
                 uuid_eq(c.uuid128, S2_UUID_INPUT_JOYCON_L) || uuid_eq(c.uuid128, S2_UUID_INPUT_JOYCON_R)) {
            b->ch_power = c;
            b->have_power = true;
        }
        break;
    }
    case GATT_EVENT_QUERY_COMPLETE: {
        int li = link_of(gatt_event_query_complete_get_handle(packet));
        if (li < 0) break;
        uint8_t l = (uint8_t)li;
        blink_t *b = &s_links[l];
        uint8_t status = gatt_event_query_complete_get_att_status(packet);
        if (b->req_pending) {
            b->req_pending = false;
            if (status != ATT_ERROR_SUCCESS) LOG("s2: link %u command write rejected, ATT status 0x%02x", l, status);
            if (b->input_cccd_wanted) {
                b->input_cccd_wanted = false;
                start_input_cccd(l);
            }
            break;
        }
        switch (b->phase) {
        case GP_SERVICES:
            if (!b->have_service) {
                gatt_fail(l, "Switch 2 HID service not found");
                break;
            }
            b->phase = GP_CHARACTERISTICS;
            gatt_client_discover_characteristics_for_service(gatt_handler, b->con, &b->service);
            break;
        case GP_CHARACTERISTICS:
            if (!b->have_input || !b->have_cmd || !b->have_cmd_rsp) {
                gatt_fail(l, "required characteristics missing");
                break;
            }
            b->cmd_use_req = (b->ch_cmd.properties & ATT_PROPERTY_WRITE) != 0;
            LOG("s2: link %u handles input=%04x cmd=%04x rsp=%04x vib=%04x power=%04x, props cmd=%02x rsp=%02x vib=%02x",
                l, b->ch_input.value_handle, b->ch_cmd.value_handle, b->ch_cmd_rsp.value_handle,
                b->have_vib ? b->ch_vib.value_handle : 0, b->have_power ? b->ch_power.value_handle : 0,
                b->ch_cmd.properties, b->ch_cmd_rsp.properties, b->have_vib ? b->ch_vib.properties : 0);
            LOG("s2: link %u commands use %s", l, b->cmd_use_req ? "write requests" : "write commands");
            b->phase = GP_CMD_CCCD;
            gatt_client_listen_for_characteristic_value_updates(&b->notif_cmd, gatt_handler, b->con, &b->ch_cmd_rsp);
            if (write_cccd(b, &b->ch_cmd_rsp) != ERROR_CODE_SUCCESS) gatt_fail(l, "could not enable command responses");
            break;
        case GP_CMD_CCCD:
            if (status != ATT_ERROR_SUCCESS) {
                gatt_fail(l, "could not enable command responses");
                break;
            }
            b->phase = GP_IDLE;
            s2c_on_gatt_ready(l, true, NULL);
            break;
        case GP_INPUT_CCCD:
            b->phase = GP_IDLE;
            s2c_on_input_enabled(l, status == ATT_ERROR_SUCCESS);
            break;
        case GP_POWER_ON:
            if (status != ATT_ERROR_SUCCESS) {
                b->phase = GP_IDLE;
                gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_power);
                s2c_on_power_info(l, false, 0);
                break;
            }
            b->phase = GP_POWER_WAIT;   // s2t_task() gives up after a second
            b->power_deadline = platform_deadline_ms(1000);
            break;
        case GP_POWER_OFF:
            b->phase = GP_IDLE;
            gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_power);
            s2c_on_power_info(l, b->power_got, b->power_info);
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

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) s2c_on_stack_ready();
        break;
    case GAP_EVENT_ADVERTISING_REPORT: {
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);
        s2c_on_advertisement(addr, gap_event_advertising_report_get_address_type(packet),
                             (int8_t)gap_event_advertising_report_get_rssi(packet),
                             gap_event_advertising_report_get_data(packet),
                             gap_event_advertising_report_get_data_length(packet));
        break;
    }
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE && s_connecting < 0) {
            // Completed after the attempt was cancelled: nobody owns it.
            if (gap_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(packet));
            }
            break;
        }
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE && s_connecting >= 0) {
            uint8_t l = (uint8_t)s_connecting;
            s_connecting = -1;
            uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                s2c_on_connect_failed(l, status);
                break;
            }
            reset_gatt(l);
            blink_t *b = &s_links[l];
            b->con = gap_subevent_le_connection_complete_get_connection_handle(packet);
            s2c_on_link_up(l, gap_subevent_le_connection_complete_get_conn_interval(packet));
            b->phase = GP_SERVICES;
            gatt_client_discover_primary_services_by_uuid128(gatt_handler, b->con, S2_UUID_HID_SERVICE);
        }
        break;
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
            int l = link_of(hci_subevent_le_connection_update_complete_get_connection_handle(packet));
            if (l >= 0) s2c_on_conn_interval((uint8_t)l, hci_subevent_le_connection_update_complete_get_conn_interval(packet));
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        int l = link_of(hci_event_disconnection_complete_get_connection_handle(packet));
        if (l >= 0) {
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            reset_gatt((uint8_t)l);
            s2c_on_disconnected((uint8_t)l, reason);
        }
        break;
    }
    default:
        break;
    }
}

static void smp_ignore_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == SM_DATA_PACKET && size > 0) {
        LOG("s2: ignoring SMP PDU 0x%02x from controller", packet[0]);
    }
}

// ---------------------------------------------------------------------------
// s2_transport.h
// ---------------------------------------------------------------------------
void s2t_init(void) {
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(0);
    // Switch 2 controllers drop the link (and power off) when standard SMP
    // pairing is attempted. BTstack's central role would answer a Security
    // Request from the peripheral by starting SMP pairing, so we take over the
    // SMP channel and silently ignore everything on it. Nintendo's own pairing
    // runs over the command characteristic instead.
    l2cap_register_fixed_channel(smp_ignore_handler, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);
    gatt_client_init();
    gatt_client_set_required_security_level(LEVEL_0);

    for (uint8_t l = 0; l < S2T_LINKS; l++) s_links[l].con = HCI_CON_HANDLE_INVALID;
    s_hci_cb.callback = hci_handler;
    hci_add_event_handler(&s_hci_cb);
    hci_power_control(HCI_POWER_ON);
}

void s2t_task(void) {
    // BTstack runs from cyw43_arch_poll() in the main loop.
    for (uint8_t l = 0; l < S2T_LINKS; l++) {
        if (s_links[l].phase == GP_POWER_WAIT && platform_time_reached(s_links[l].power_deadline)) power_unsubscribe(l);
    }
}

bool s2t_poll_power(uint8_t l) {
    if (l >= S2T_LINKS) return false;
    blink_t *b = &s_links[l];
    if (b->con == HCI_CON_HANDLE_INVALID || !b->have_power || b->phase != GP_IDLE || b->req_pending) return false;
    b->power_got = false;
    b->phase = GP_POWER_ON;
    gatt_client_listen_for_characteristic_value_updates(&b->notif_power, gatt_handler, b->con, &b->ch_power);
    if (write_cccd(b, &b->ch_power) != ERROR_CODE_SUCCESS) {
        gatt_client_stop_listening_for_characteristic_value_updates(&b->notif_power);
        b->phase = GP_IDLE;
        return false;
    }
    return true;
}

bool s2t_ready(void) {
    return hci_get_state() == HCI_STATE_WORKING;
}

void s2t_start_scan(bool low_duty) {
    // Passive scan. The CYW43 shares its radio between Bluetooth and Wi-Fi, so
    // scan at 50% duty normally and much less while the configuration access
    // point is up (or a Joy-Con 2 is connected), otherwise the others get
    // almost no airtime.
    gap_stop_scan();
    if (low_duty) gap_set_scan_parameters(0, 0x0200, 0x0030);
    else gap_set_scan_parameters(0, 0x0060, 0x0030);
    gap_start_scan();
}

void s2t_stop_scan(void) {
    gap_stop_scan();
}

bool s2t_connect(uint8_t l, const uint8_t addr[6], uint8_t addr_type) {
    if (l >= S2T_LINKS || s_connecting >= 0) return false;
    bd_addr_t a;
    memcpy(a, addr, 6);
    gap_set_connection_parameters(0x0030, 0x0030, CONN_INTERVAL_MIN, CONN_INTERVAL_MAX, 0, CONN_SUPERVISION, 0, 0);
    if (gap_connect(a, (bd_addr_type_t)addr_type) != ERROR_CODE_SUCCESS) return false;
    s_connecting = l;
    return true;
}

void s2t_cancel_connect(void) {
    s_connecting = -1;
    gap_connect_cancel();
}

void s2t_disconnect(uint8_t l) {
    if (l < S2T_LINKS && s_links[l].con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_links[l].con);
}

bool s2t_has_char(uint8_t l, s2t_char_t ch) {
    if (l >= S2T_LINKS) return false;
    return ch == S2T_CHAR_COMMAND ? s_links[l].have_cmd : s_links[l].have_vib;
}

s2t_write_result_t s2t_write(uint8_t l, s2t_char_t ch, const uint8_t *data, uint16_t len) {
    if (l >= S2T_LINKS) return S2T_WRITE_ERROR;
    blink_t *b = &s_links[l];
    if (b->con == HCI_CON_HANDLE_INVALID || !s2t_has_char(l, ch)) return S2T_WRITE_ERROR;
    uint16_t handle = ch == S2T_CHAR_COMMAND ? b->ch_cmd.value_handle : b->ch_vib.value_handle;
    if (ch == S2T_CHAR_COMMAND && b->cmd_use_req) {
        if (b->req_pending || b->phase != GP_IDLE) return S2T_WRITE_BUSY;
        if (len > sizeof b->req_buf) return S2T_WRITE_ERROR;
        memcpy(b->req_buf, data, len);
        uint8_t st = gatt_client_write_value_of_characteristic(gatt_handler, b->con, handle, len, b->req_buf);
        if (st == ERROR_CODE_SUCCESS) {
            b->req_pending = true;
            return S2T_WRITE_OK;
        }
        if (st == GATT_CLIENT_VALUE_TOO_LONG) return S2T_WRITE_ERROR;
        return S2T_WRITE_BUSY;
    }
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(b->con, handle, len, (uint8_t *)data);
    if (st == ERROR_CODE_SUCCESS) return S2T_WRITE_OK;
    if (st == GATT_CLIENT_VALUE_TOO_LONG) return S2T_WRITE_ERROR;
    return S2T_WRITE_BUSY;
}

static void start_input_cccd(uint8_t l) {
    blink_t *b = &s_links[l];
    b->phase = GP_INPUT_CCCD;
    gatt_client_listen_for_characteristic_value_updates(&b->notif_input, gatt_handler, b->con, &b->ch_input);
    if (write_cccd(b, &b->ch_input) != ERROR_CODE_SUCCESS) {
        b->phase = GP_IDLE;
        s2c_on_input_enabled(l, false);
    }
}

void s2t_local_address(uint8_t out[6]) {
    bd_addr_t a;
    gap_local_bd_addr(a);
    memcpy(out, a, 6);
}

uint16_t s2t_mtu(uint8_t l) {
    uint16_t mtu = 23;
    if (l < S2T_LINKS && s_links[l].con != HCI_CON_HANDLE_INVALID) gatt_client_get_mtu(s_links[l].con, &mtu);
    return mtu;
}

void s2t_enable_input(uint8_t l) {
    if (l >= S2T_LINKS) return;
    // BTstack runs one GATT request at a time per connection.
    if (s_links[l].req_pending) s_links[l].input_cccd_wanted = true;
    else start_input_cccd(l);
}
