#include "s2_proto.h"

#include <string.h>

const uint8_t S2_UUID_HID_SERVICE[16] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0};
const uint8_t S2_UUID_INPUT_COMMON[16] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2};
const uint8_t S2_UUID_CMD_WRITE[16] = {
    0x64, 0x9d, 0x4a, 0xc9, 0x8e, 0xb7, 0x4e, 0x6c, 0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05};
const uint8_t S2_UUID_CMD_RESPONSE[16] = {
    0xc7, 0x65, 0xa9, 0x61, 0xd9, 0xd8, 0x4d, 0x36, 0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a};
const uint8_t S2_UUID_VIB_PRO[16] = {
    0xcc, 0x48, 0x3f, 0x51, 0x92, 0x58, 0x42, 0x7d, 0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05};
const uint8_t S2_UUID_VIB_GC[16] = {
    0x3f, 0x8f, 0xb6, 0x70, 0xab, 0x25, 0x45, 0xbf, 0xb5, 0x40, 0x38, 0xc7, 0x28, 0x34, 0xd0, 0x64};
const uint8_t S2_UUID_INPUT_PRO[16] = {
    0x74, 0x92, 0x86, 0x6c, 0xec, 0x3e, 0x46, 0x19, 0x82, 0x58, 0x32, 0x75, 0x5f, 0xfc, 0xc0, 0xf8};
const uint8_t S2_UUID_INPUT_GC[16] = {
    0x82, 0x61, 0xcb, 0xa1, 0x94, 0x35, 0x42, 0x0c, 0x84, 0xd6, 0xf0, 0xc7, 0x5a, 0x2c, 0x8e, 0x4d};
const uint8_t S2_UUID_INPUT_JOYCON_L[16] = {
    0xcc, 0x1b, 0xbb, 0xb5, 0x73, 0x54, 0x4d, 0x32, 0xa7, 0x16, 0xa8, 0x1c, 0xb2, 0x41, 0xa3, 0x2a};
const uint8_t S2_UUID_INPUT_JOYCON_R[16] = {
    0xd5, 0xa9, 0xe0, 0x1e, 0x2f, 0xfc, 0x4c, 0xca, 0xb2, 0x0c, 0x8b, 0x67, 0x14, 0x2b, 0xf4, 0x42};
const uint8_t S2_UUID_VIB_JOYCON_L[16] = {
    0x28, 0x93, 0x26, 0xcb, 0xa4, 0x71, 0x48, 0x5d, 0xa8, 0xf4, 0x24, 0x0c, 0x14, 0xf1, 0x82, 0x41};
const uint8_t S2_UUID_VIB_JOYCON_R[16] = {
    0xfa, 0x19, 0xb0, 0xfb, 0xcd, 0x1f, 0x46, 0xa7, 0x84, 0xa1, 0xbb, 0xb0, 0x9e, 0x00, 0xc1, 0x49};
const uint8_t S2_UUID_REPORT_RATE_DESC[16] = {
    0x67, 0x9d, 0x55, 0x10, 0x5a, 0x24, 0x4d, 0xee, 0x95, 0x57, 0x95, 0xdf, 0x80, 0x48, 0x6e, 0xcb};
const uint8_t S2_UUID_UNKNOWN_SVC_WRITE[16] = {
    0x00, 0xc5, 0xaf, 0x5d, 0x19, 0x64, 0x4e, 0x30, 0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x82};

// 5CF6EE792CDF05E1BA2B6325C41A5F10, as documented (it is sent byte reversed).
const uint8_t S2_PAIR_DEVICE_KEY_B1[16] = {
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1, 0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool s2_parse_manufacturer_data(const uint8_t *data, size_t len, s2_adv_info_t *out) {
    // Offsets (including the 2 byte company id):
    //   0 company id (53 05), 2..4 constants, 5 vid, 7 pid, 0xB wake flag,
    //   0xC host address (reversed), 0x12 constant 0x0F
    if (len < 0x12) return false;
    if (rd16(data) != S2_NINTENDO_COMPANY_ID) return false;
    memset(out, 0, sizeof *out);
    out->vid = rd16(data + 5);
    out->pid = rd16(data + 7);
    if (out->vid != S2_NINTENDO_VID) return false;
    out->wake_console = data[0xB] == 0x81;
    memcpy(out->host_addr_le, data + 0xC, 6);
    bool all_zero = true;
    for (int i = 0; i < 6; i++) {
        if (out->host_addr_le[i] != 0) all_zero = false;
    }
    out->pairing_mode = all_zero;
    return true;
}

size_t s2_build_command(uint8_t *out, size_t out_cap, uint8_t cmd, uint8_t sub,
                        const uint8_t *data, size_t data_len) {
    if (out_cap < S2_CMD_HEADER_LEN + data_len || data_len > 0xFF) return 0;
    out[0] = cmd;
    out[1] = 0x91;          // host -> device
    out[2] = 0x01;          // transport: Bluetooth
    out[3] = sub;
    out[4] = 0x00;
    out[5] = (uint8_t)data_len;
    out[6] = 0x00;
    out[7] = 0x00;
    if (data_len) memcpy(out + S2_CMD_HEADER_LEN, data, data_len);
    return S2_CMD_HEADER_LEN + data_len;
}

size_t s2_build_memory_read(uint8_t *out, size_t out_cap, uint32_t address, uint8_t length) {
    uint8_t d[8] = {length, 0x7e, 0x00, 0x00,
                    (uint8_t)address, (uint8_t)(address >> 8), (uint8_t)(address >> 16), (uint8_t)(address >> 24)};
    return s2_build_command(out, out_cap, S2_CMD_MEMORY, S2_SUB_MEMORY_READ, d, sizeof d);
}

bool s2_parse_response(const uint8_t *buf, size_t len, s2_response_t *out) {
    if (len < S2_CMD_HEADER_LEN) return false;
    if (buf[1] != 0x01) return false;   // device -> host
    out->cmd = buf[0];
    out->sub = buf[3];
    out->ack = buf[5];
    out->data = buf + S2_CMD_HEADER_LEN;
    out->data_len = len - S2_CMD_HEADER_LEN;
    return true;
}

bool s2_parse_memory_read(const s2_response_t *rsp, uint32_t address, uint8_t length,
                          const uint8_t **payload) {
    if (rsp->cmd != S2_CMD_MEMORY || rsp->sub != S2_SUB_MEMORY_READ) return false;
    if (rsp->data_len < 8u + length) return false;
    if (rsp->data[0] != length || rd32(rsp->data + 4) != address) return false;
    *payload = rsp->data + 8;
    return true;
}

void s2_unpack12(const uint8_t b[3], uint16_t *a, uint16_t *c) {
    *a = (uint16_t)(b[0] | ((b[1] & 0x0F) << 8));
    *c = (uint16_t)((b[1] >> 4) | (b[2] << 4));
}

bool s2_parse_input_report(const uint8_t *d, size_t len, s2_input_t *out) {
    if (len < S2_INPUT_REPORT_MIN_LEN) return false;
    out->counter = rd32(d + 0x00);
    out->buttons = rd32(d + 0x04);
    s2_unpack12(d + 0x0A, &out->stick_l[0], &out->stick_l[1]);
    s2_unpack12(d + 0x0D, &out->stick_r[0], &out->stick_r[1]);
    out->battery_mv = rd16(d + 0x1F);
    out->charge_state = d[0x21];
    out->imu_timestamp = rd32(d + 0x2A);
    out->temperature = (int16_t)rd16(d + 0x2E);
    for (int i = 0; i < 3; i++) {
        out->accel[i] = (int16_t)rd16(d + 0x30 + 2 * i);
        out->gyro[i] = (int16_t)rd16(d + 0x36 + 2 * i);
    }
    out->mouse_x = rd16(d + 0x10);
    out->mouse_y = rd16(d + 0x12);
    out->mouse_quality = rd16(d + 0x14);
    out->mouse_distance = rd16(d + 0x16);
    out->trigger_l = len > 0x3C ? d[0x3C] : 0;
    out->trigger_r = len > 0x3D ? d[0x3D] : 0;
    return true;
}

void s2_parse_stick_cal(const uint8_t data[9], s2_stick_cal_t *out) {
    s2_unpack12(data + 0, &out->center[0], &out->center[1]);
    s2_unpack12(data + 3, &out->max[0], &out->max[1]);
    s2_unpack12(data + 6, &out->min[0], &out->min[1]);
    out->valid = out->center[0] && out->center[1] && out->max[0] && out->max[1] &&
                 out->min[0] && out->min[1] && out->center[0] != 0xFFF;
}

void s2_default_stick_cal(s2_stick_cal_t *out) {
    out->center[0] = out->center[1] = 2048;
    out->max[0] = out->max[1] = 1400;
    out->min[0] = out->min[1] = 1400;
    out->valid = true;
}

float s2_stick_axis(const s2_stick_cal_t *cal, int axis, uint16_t raw) {
    float v = (float)raw - (float)cal->center[axis];
    if (v >= 0) {
        v /= (float)(cal->max[axis] ? cal->max[axis] : 1);
    } else {
        v /= (float)(cal->min[axis] ? cal->min[axis] : 1);
    }
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return v;
}

uint8_t s2_led_pattern_for_player(int player) {
    static const uint8_t pattern[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    if (player < 1) player = 1;
    if (player > 8) player = 8;
    return pattern[player - 1];
}
