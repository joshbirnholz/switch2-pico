// Fixed 48 kHz UAC2 function, with independent playback/capture clocks and
// read-only physical connector controls. No host or board dependencies.
#include "usb_audio2.h"
#include <string.h>
#define LE16(x) (uint8_t)(x), (uint8_t)((x) >> 8)
#define LE32(x) LE16(x), LE16((uint32_t)(x) >> 16)
#if S2_HEADSET_MIC
#define AC_LENGTH (9 + 8 + 17 + 12 + 8 + 17 + 12)
#else
#define AC_LENGTH (9 + 8 + 17 + 12)
#endif

uint16_t audio2_descriptor(uint8_t *buf, uint16_t cap, uint8_t itf,
                          uint8_t ep_out, uint8_t ep_in, uint8_t ep_notify, uint8_t str) {
    (void)ep_in;
    const uint8_t as = itf + 1;
    const uint8_t d[] = {
        8, 11, itf, AUDIO2_INTERFACE_COUNT, 1, 0, 0x20, str, // IAD
        9, 4, itf, 0, 1, 1, 1, 0x20, str, // AudioControl + interrupt endpoint
        9, 0x24, 1, LE16(0x0200), 4, LE16(AC_LENGTH), 0, // headset category
        // Playback is adapted to USB SOF; capture follows the BLE sample clock.
        8, 0x24, 10, AUDIO2_CLOCK_PLAYBACK, 5, 5, 0, 0,
        17, 0x24, 2, AUDIO2_TERMINAL_USB_OUT, LE16(0x0101), 0,
        AUDIO2_CLOCK_PLAYBACK, 2, LE32(3), 0, LE16(0), 0,
        12, 0x24, 3, AUDIO2_TERMINAL_HEADPHONES, LE16(0x0302), 0,
        AUDIO2_TERMINAL_USB_OUT, AUDIO2_CLOCK_PLAYBACK, LE16(4), 0,
#if S2_HEADSET_MIC
        8, 0x24, 10, AUDIO2_CLOCK_CAPTURE, 1, 5, 0, 0,
        17, 0x24, 2, AUDIO2_TERMINAL_MIC, LE16(0x0201), 0,
        AUDIO2_CLOCK_CAPTURE, 1, LE32(0), 0, LE16(4), 0,
        12, 0x24, 3, AUDIO2_TERMINAL_USB_IN, LE16(0x0101), 0,
        AUDIO2_TERMINAL_MIC, AUDIO2_CLOCK_CAPTURE, LE16(0), 0,
#endif
        7, 5, ep_notify, 3, LE16(6), 8,
        9, 4, as, 0, 0, 1, 2, 0x20, 0,
        9, 4, as, 1, 1, 1, 2, 0x20, 0,
        16, 0x24, 1, AUDIO2_TERMINAL_USB_OUT, 0, 1, LE32(1), 2, LE32(3), 0,
        6, 0x24, 2, 1, 2, 16,
        7, 5, ep_out, 0x09, LE16(196), 1, // adaptive OUT
        8, 0x25, 1, 0, 0, 0, LE16(0),
#if S2_HEADSET_MIC
        9, 4, (uint8_t)(as + 1), 0, 0, 1, 2, 0x20, 0,
        9, 4, (uint8_t)(as + 1), 1, 1, 1, 2, 0x20, 0,
        16, 0x24, 1, AUDIO2_TERMINAL_USB_IN, 0, 1, LE32(1), 1, LE32(0), 0,
        6, 0x24, 2, 1, 2, 16,
        7, 5, ep_in, 0x05, LE16(98), 1, // asynchronous IN
        8, 0x25, 1, 0, 0, 0, LE16(0),
#endif
    };
    _Static_assert(sizeof d == AUDIO2_DESCRIPTOR_LENGTH, "UAC2 descriptor length");
    if (cap < sizeof d) return 0;
    memcpy(buf, d, sizeof d);
    return sizeof d;
}

uint16_t audio2_get(uint8_t entity, uint8_t selector, uint8_t channel, uint8_t request,
                    bool headphones, bool microphone, uint8_t *out, uint16_t cap) {
    uint8_t reply[14] = {0};
    uint16_t n = 0;
    if (channel) return 0;
    bool clock = entity == AUDIO2_CLOCK_PLAYBACK;
#if S2_HEADSET_MIC
    clock = clock || entity == AUDIO2_CLOCK_CAPTURE;
#endif
    if (clock && selector == 1) {
        if (request == 1) {
            const uint8_t rate[] = {LE32(48000)};
            memcpy(reply, rate, sizeof rate); n = sizeof rate;
        } else if (request == 2) {
            const uint8_t range[] = {LE16(1), LE32(48000), LE32(48000), LE32(0)};
            memcpy(reply, range, sizeof range); n = sizeof range;
        }
    } else if (clock && selector == 2 && request == 1) {
        reply[0] = 1; n = 1; // clock valid even with no BLE controller
    } else if (selector == AUDIO2_CONNECTOR_CONTROL && request == 1) {
        if (entity == AUDIO2_TERMINAL_HEADPHONES) {
            reply[0] = headphones ? 2 : 0;
            reply[1] = headphones ? 3 : 0; // bmChannelConfig, front left/right
            n = 6; // bNrChannels, bmChannelConfig (4), iChannelNames
#if S2_HEADSET_MIC
        } else if (entity == AUDIO2_TERMINAL_MIC) {
            reply[0] = microphone ? 1 : 0; n = 6;
#endif
        }
    }
    if (cap < n) return 0;
    memcpy(out, reply, n);
    return n;
}

void audio2_notification(uint8_t out[6], uint8_t itf, uint8_t terminal) {
    // Interface entity CUR changed: bInfo, bAttribute, wValue, wIndex.
    const uint8_t msg[] = {0, 1, 0, AUDIO2_CONNECTOR_CONTROL, itf, terminal};
    memcpy(out, msg, sizeof msg);
}
