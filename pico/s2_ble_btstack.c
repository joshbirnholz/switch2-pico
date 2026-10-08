// Bluetooth LE transport for the Switch 2 controller on the Pico W / Pico 2 W
// (BTstack on the CYW43439). See core/src/s2_transport.h.

#include <string.h>

#include "btstack.h"

#include "log.h"
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
} gatt_phase_t;

static btstack_packet_callback_registration_t s_hci_cb;
static hci_con_handle_t s_con = HCI_CON_HANDLE_INVALID;
static bool s_connecting;
static gatt_phase_t s_phase;

static gatt_client_service_t s_service;
static bool s_have_service;
static gatt_client_characteristic_t s_ch_input, s_ch_cmd, s_ch_cmd_rsp, s_ch_vib;
static bool s_have_input, s_have_cmd, s_have_cmd_rsp, s_have_vib;
static gatt_client_notification_t s_notif_input, s_notif_cmd;

// Commands go out as ATT Write Requests when the characteristic allows it
// (the controller ignores Write Commands there). BTstack runs one request at a
// time and needs the buffer until it completes.
static bool s_cmd_use_req;
static bool s_req_pending;
static bool s_input_cccd_wanted;   // enable input once the pending write completes
static void start_input_cccd(void);
static uint8_t s_req_buf[256];

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static bool uuid_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

static void reset_gatt(void) {
    if (s_con != HCI_CON_HANDLE_INVALID) {
        gatt_client_stop_listening_for_characteristic_value_updates(&s_notif_input);
        gatt_client_stop_listening_for_characteristic_value_updates(&s_notif_cmd);
    }
    s_con = HCI_CON_HANDLE_INVALID;
    s_phase = GP_NONE;
    s_req_pending = false;
    s_input_cccd_wanted = false;
    s_have_service = s_have_input = s_have_cmd = s_have_cmd_rsp = s_have_vib = false;
}

// Enable notifications by writing the CCCD with a Write Request at
// value_handle + 1, where these controllers put it (as the nRF52840 build
// does). BTstack's gatt_client_write_client_characteristic_configuration()
// first looks the CCCD up with a Read By Type request, which the controller
// never answers, so the write never happened.
static uint8_t s_cccd_value[2] = {0x01, 0x00};   // notifications on

static uint8_t write_cccd(gatt_client_characteristic_t *c) {
    return gatt_client_write_value_of_characteristic(gatt_handler, s_con, (uint16_t)(c->value_handle + 1),
                                                     sizeof s_cccd_value, s_cccd_value);
}

static void gatt_fail(const char *why) {
    s_phase = GP_IDLE;
    s2c_on_gatt_ready(false, why);
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
        if (s_have_input && h == s_ch_input.value_handle) s2c_on_input_report(v, len);
        else if (s_have_cmd_rsp && h == s_ch_cmd_rsp.value_handle) s2c_on_command_response(v, len);
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
    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t status = gatt_event_query_complete_get_att_status(packet);
        if (s_req_pending) {
            s_req_pending = false;
            if (status != ATT_ERROR_SUCCESS) LOG("s2: command write rejected, ATT status 0x%02x", status);
            if (s_input_cccd_wanted) {
                s_input_cccd_wanted = false;
                start_input_cccd();
            }
            break;
        }
        switch (s_phase) {
        case GP_SERVICES:
            if (!s_have_service) {
                gatt_fail("Switch 2 HID service not found");
                break;
            }
            s_phase = GP_CHARACTERISTICS;
            gatt_client_discover_characteristics_for_service(gatt_handler, s_con, &s_service);
            break;
        case GP_CHARACTERISTICS:
            if (!s_have_input || !s_have_cmd || !s_have_cmd_rsp) {
                gatt_fail("required characteristics missing");
                break;
            }
            s_cmd_use_req = (s_ch_cmd.properties & ATT_PROPERTY_WRITE) != 0;
            LOG("s2: handles input=%04x cmd=%04x rsp=%04x vib=%04x, props cmd=%02x rsp=%02x vib=%02x",
                s_ch_input.value_handle, s_ch_cmd.value_handle, s_ch_cmd_rsp.value_handle,
                s_have_vib ? s_ch_vib.value_handle : 0, s_ch_cmd.properties, s_ch_cmd_rsp.properties,
                s_have_vib ? s_ch_vib.properties : 0);
            LOG("s2: commands use %s", s_cmd_use_req ? "write requests" : "write commands");
            s_phase = GP_CMD_CCCD;
            gatt_client_listen_for_characteristic_value_updates(&s_notif_cmd, gatt_handler, s_con, &s_ch_cmd_rsp);
            if (write_cccd(&s_ch_cmd_rsp) != ERROR_CODE_SUCCESS) gatt_fail("could not enable command responses");
            break;
        case GP_CMD_CCCD:
            if (status != ATT_ERROR_SUCCESS) {
                gatt_fail("could not enable command responses");
                break;
            }
            s_phase = GP_IDLE;
            s2c_on_gatt_ready(true, NULL);
            break;
        case GP_INPUT_CCCD:
            s_phase = GP_IDLE;
            s2c_on_input_enabled(status == ATT_ERROR_SUCCESS);
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
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE && s_connecting) {
            s_connecting = false;
            uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                s2c_on_connect_failed(status);
                break;
            }
            reset_gatt();
            s_con = gap_subevent_le_connection_complete_get_connection_handle(packet);
            s2c_on_link_up(gap_subevent_le_connection_complete_get_conn_interval(packet));
            s_phase = GP_SERVICES;
            gatt_client_discover_primary_services_by_uuid128(gatt_handler, s_con, S2_UUID_HID_SERVICE);
        }
        break;
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
            s2c_on_conn_interval(hci_subevent_le_connection_update_complete_get_conn_interval(packet));
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == s_con) {
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            reset_gatt();
            s2c_on_disconnected(reason);
        }
        break;
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

    s_hci_cb.callback = hci_handler;
    hci_add_event_handler(&s_hci_cb);
    hci_power_control(HCI_POWER_ON);
}

void s2t_task(void) {
    // BTstack runs from cyw43_arch_poll() in the main loop.
}

bool s2t_ready(void) {
    return hci_get_state() == HCI_STATE_WORKING;
}

void s2t_start_scan(bool low_duty) {
    // Passive scan. The CYW43 shares its radio between Bluetooth and Wi-Fi, so
    // scan at 50% duty normally and much less while the configuration access
    // point is up, otherwise Wi-Fi clients get almost no airtime.
    gap_stop_scan();
    if (low_duty) gap_set_scan_parameters(0, 0x0200, 0x0030);
    else gap_set_scan_parameters(0, 0x0060, 0x0030);
    gap_start_scan();
}

void s2t_stop_scan(void) {
    gap_stop_scan();
}

bool s2t_connect(const uint8_t addr[6], uint8_t addr_type) {
    bd_addr_t a;
    memcpy(a, addr, 6);
    gap_set_connection_parameters(0x0030, 0x0030, CONN_INTERVAL_MIN, CONN_INTERVAL_MAX, 0, CONN_SUPERVISION, 0, 0);
    if (gap_connect(a, (bd_addr_type_t)addr_type) != ERROR_CODE_SUCCESS) return false;
    s_connecting = true;
    return true;
}

void s2t_cancel_connect(void) {
    s_connecting = false;
    gap_connect_cancel();
}

void s2t_disconnect(void) {
    if (s_con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con);
}

bool s2t_has_char(s2t_char_t ch) {
    return ch == S2T_CHAR_COMMAND ? s_have_cmd : s_have_vib;
}

s2t_write_result_t s2t_write(s2t_char_t ch, const uint8_t *data, uint16_t len) {
    if (s_con == HCI_CON_HANDLE_INVALID || !s2t_has_char(ch)) return S2T_WRITE_ERROR;
    uint16_t handle = ch == S2T_CHAR_COMMAND ? s_ch_cmd.value_handle : s_ch_vib.value_handle;
    if (ch == S2T_CHAR_COMMAND && s_cmd_use_req) {
        if (s_req_pending || s_phase != GP_IDLE) return S2T_WRITE_BUSY;
        if (len > sizeof s_req_buf) return S2T_WRITE_ERROR;
        memcpy(s_req_buf, data, len);
        uint8_t st = gatt_client_write_value_of_characteristic(gatt_handler, s_con, handle, len, s_req_buf);
        if (st == ERROR_CODE_SUCCESS) {
            s_req_pending = true;
            return S2T_WRITE_OK;
        }
        if (st == GATT_CLIENT_VALUE_TOO_LONG) return S2T_WRITE_ERROR;
        return S2T_WRITE_BUSY;
    }
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(s_con, handle, len, (uint8_t *)data);
    if (st == ERROR_CODE_SUCCESS) return S2T_WRITE_OK;
    if (st == GATT_CLIENT_VALUE_TOO_LONG) return S2T_WRITE_ERROR;
    return S2T_WRITE_BUSY;
}

static void start_input_cccd(void) {
    s_phase = GP_INPUT_CCCD;
    gatt_client_listen_for_characteristic_value_updates(&s_notif_input, gatt_handler, s_con, &s_ch_input);
    if (write_cccd(&s_ch_input) != ERROR_CODE_SUCCESS) {
        s_phase = GP_IDLE;
        s2c_on_input_enabled(false);
    }
}

void s2t_local_address(uint8_t out[6]) {
    bd_addr_t a;
    gap_local_bd_addr(a);
    memcpy(out, a, 6);
}

uint16_t s2t_mtu(void) {
    uint16_t mtu = 23;
    if (s_con != HCI_CON_HANDLE_INVALID) gatt_client_get_mtu(s_con, &mtu);
    return mtu;
}

void s2t_enable_input(void) {
    // BTstack runs one GATT request at a time.
    if (s_req_pending) s_input_cccd_wanted = true;
    else start_input_cccd();
}
