// USB speaker for the Pro Controller 2's headphone jack (see usb_audio.h).
//
// USB Audio Class 1.0 rather than 2.0: every host has a driver for it, and
// a fixed 48 kHz stereo stream needs nothing 2.0 adds. There's no feature
// unit, so the host scales the volume itself (the controller has no volume
// control either: research/headset-audio.md). The endpoint is adaptive: the
// host sends at its own clock, and headset_audio_pico.c absorbs the drift.
//
// TinyUSB only queues the next receive from tud_task(), and the main loop is
// often busy for longer than the 1 ms between packets (Bluetooth, Wi-Fi):
// isochronous packets that find no buffer are lost. So we run the endpoint
// ourselves: double buffered, with no buffer interrupt, emptied and armed
// again at every start of frame (the class driver's SOF callback, in the
// USB interrupt). Each buffer has a whole frame of slack.
//
// Without a buffer interrupt the endpoint never shows in BUFF_STATUS, so
// TinyUSB's interrupt handler never sees it. (An earlier version took the
// packets in its own buffer interrupt handler ahead of TinyUSB's. A packet
// completing between the two handlers' reads of BUFF_STATUS reached TinyUSB,
// which panics on an endpoint it has no transfer for: the main loop stopped
// and the watchdog reset the dongle, minutes into a game.)

#include "usb_audio.h"

#include <string.h>

#include "hardware/structs/usb.h"
#include "tusb.h"

#include "headset_audio_pico.h"
#include "settings.h"
#include "usb_mode.h"

#define CHANNELS      2
#define SAMPLE_BYTES  2
#define RATE          48000
// One more sample per channel than 1 ms holds: an adaptive endpoint's host
// may send 49 now and then.
#define EP_SIZE       ((RATE / 1000 + 1) * CHANNELS * SAMPLE_BYTES)
#define AC_CS_LEN     (9 + 12 + 9)   // header, input terminal, output terminal

// Buffer 1 starts 256 bytes after buffer 0 (the ISO offset field of the
// buffer control register); allocate room for both.
#define BUF1_OFFSET   256u
#define ISO_OFFSET_256 (1u << 27)
#define EP_ALLOC      512u

#define TERMINAL_USB_STREAMING 1
#define TERMINAL_HEADPHONES    2

static struct {
    uint8_t itf_ac;   // AudioStreaming is itf_ac + 1
    uint8_t ep_out;
    volatile uint8_t alt;
    volatile bool running;   // the endpoint is set up and armed by us
    uint8_t next;            // the buffer the next packet goes to (0, 1)
    tusb_desc_endpoint_t const *ep_desc;
    // The endpoint's buffer control register and buffers in the USB RAM.
    io_rw_32 *buf_ctrl;
    const volatile uint32_t *buf;
} s_audio;

CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_ctrl[4];

// Write the buffer control register: the bits, then 12 cycles later the
// same with AVAIL (RP2350 datasheet, USB "concurrent access").
static void __not_in_flash_func(buf_ctrl_set)(uint32_t v, uint32_t avail) {
    *s_audio.buf_ctrl = v;
    busy_wait_at_least_cycles(12);
    *s_audio.buf_ctrl = v | avail;
}

static void arm_both(void) {
    // Isochronous: always DATA0. SEL: start with buffer 0.
    buf_ctrl_set(ISO_OFFSET_256 | USB_BUF_CTRL_SEL | EP_SIZE | EP_SIZE << 16,
                 USB_BUF_CTRL_AVAIL | USB_BUF_CTRL_AVAIL << 16);
    s_audio.next = 0;
}

// Take buffer b's packet and arm it again. Done right after a start of
// frame: the other buffer may be armed, but its packet (130 us on the wire)
// can't have completed yet, so rewriting its half of the register is safe.
static void __not_in_flash_func(take)(uint32_t bc, int b) {
    uint32_t shift = b ? 16u : 0u;
    uint32_t len = (bc >> shift) & USB_BUF_CTRL_LEN_MASK;
    // A stereo sample is one word of the buffer (L, then R, little-endian).
    if (s_audio.alt == 1) {
        headset_audio_pcm(s_audio.buf + (b ? BUF1_OFFSET / 4 : 0), len / (CHANNELS * SAMPLE_BYTES));
    }
    uint32_t other = bc & (0xffffu << (16u - shift)) & ~ISO_OFFSET_256 & ~USB_BUF_CTRL_SEL;
    buf_ctrl_set(other | ISO_OFFSET_256 | (uint32_t)EP_SIZE << shift, USB_BUF_CTRL_AVAIL << shift);
}

// Diagnostics: start-of-frame callbacks that found both buffers full or
// were out of step, and frame numbers skipped between callbacks.
static volatile uint32_t s_both_full, s_out_of_step, s_frames_skipped;
static uint32_t s_last_frame;

void usb_audio_stats(uint32_t *both_full, uint32_t *out_of_step, uint32_t *frames_skipped) {
    *both_full = s_both_full;
    *out_of_step = s_out_of_step;
    *frames_skipped = s_frames_skipped;
    s_both_full = s_out_of_step = s_frames_skipped = 0;
}

static void __not_in_flash_func(audio_sof)(uint8_t rhport, uint32_t frame_count) {
    (void)rhport;
    if (!s_audio.running) return;
    uint32_t gap = (frame_count - s_last_frame) & 0x7ffu;
    if (gap > 1 && s_last_frame) s_frames_skipped += gap - 1;
    s_last_frame = frame_count;
    {
        uint32_t bc = *s_audio.buf_ctrl;
        if ((bc & USB_BUF_CTRL_FULL) && (bc & (USB_BUF_CTRL_FULL << 16))) s_both_full++;
    }
    // Usually one buffer is full; both after a late start of frame. In the
    // order they were filled, starting with s_audio.next.
    for (int i = 0; i < 2; i++) {
        uint32_t bc = *s_audio.buf_ctrl;
        int b = s_audio.next;
        if (!((bc >> (b ? 16 : 0)) & USB_BUF_CTRL_FULL)) {
            b ^= 1;   // out of step (a packet lost): follow the hardware
            if (!((bc >> (b ? 16 : 0)) & USB_BUF_CTRL_FULL)) return;
            s_out_of_step++;
        }
        take(bc, b);
        s_audio.next = (uint8_t)(b ^ 1);
    }
}

bool usb_audio_present(void) {
    // A Pro Controller 2 (the controller type the dongle started for) in
    // SInput mode. Latched like the USB mode: descriptors must not change.
    static int present = -1;
    if (usb_mode_config_only()) return false;
    if (present < 0) {
        present = usb_mode_active() == USB_MODE_SINPUT && settings_boot_ctrl(&g_settings) == CTRL_PRO;
    }
    return present == 1;
}

uint16_t usb_audio_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_out, uint8_t str_idx) {
    const uint8_t as = (uint8_t)(itf + 1);
    const uint8_t desc[] = {
        // Interface association: both interfaces are one function.
        8, TUSB_DESC_INTERFACE_ASSOCIATION, itf, USB_AUDIO_ITF_COUNT, TUSB_CLASS_AUDIO, 0x01, 0x00, str_idx,
        // AudioControl interface
        9, TUSB_DESC_INTERFACE, itf, 0, 0, TUSB_CLASS_AUDIO, 0x01, 0x00, str_idx,
        9, 0x24, 0x01, U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(AC_CS_LEN), 1, as,    // header, UAC 1.0
        12, 0x24, 0x02, TERMINAL_USB_STREAMING, U16_TO_U8S_LE(0x0101), 0, CHANNELS,
        U16_TO_U8S_LE(0x0003), 0, 0,                                              // input: USB, L + R
        9, 0x24, 0x03, TERMINAL_HEADPHONES, U16_TO_U8S_LE(0x0302), 0, TERMINAL_USB_STREAMING, 0,
        // AudioStreaming interface: alternate 0 has no endpoint (not streaming).
        9, TUSB_DESC_INTERFACE, as, 0, 0, TUSB_CLASS_AUDIO, 0x02, 0x00, 0,
        9, TUSB_DESC_INTERFACE, as, 1, 1, TUSB_CLASS_AUDIO, 0x02, 0x00, 0,
        7, 0x24, 0x01, TERMINAL_USB_STREAMING, 1, U16_TO_U8S_LE(0x0001),         // general: PCM
        11, 0x24, 0x02, 0x01, CHANNELS, SAMPLE_BYTES, 16, 1,
        (uint8_t)RATE, (uint8_t)(RATE >> 8), (uint8_t)(RATE >> 16),             // type I, 48 kHz
        9, TUSB_DESC_ENDPOINT, ep_out, 0x09, U16_TO_U8S_LE(EP_SIZE), 1, 0, 0,    // isochronous, adaptive
        7, 0x25, 0x01, 0x00, 0, U16_TO_U8S_LE(0),
    };
    _Static_assert(sizeof desc == USB_AUDIO_DESC_LEN, "USB_AUDIO_DESC_LEN");
    if (cap < sizeof desc) return 0;
    memcpy(buf, desc, sizeof desc);
    return sizeof desc;
}

bool usb_audio_streaming(void) {
    return s_audio.alt == 1 && tud_mounted() && !tud_suspended();
}

static void audio_init(void) {
    memset(&s_audio, 0, sizeof s_audio);
}

static bool audio_deinit(void) {
    return true;
}

static void audio_reset(uint8_t rhport) {
    (void)rhport;
    s_audio.running = false;   // before the rest, for audio_sof()
    memset(&s_audio, 0, sizeof s_audio);
}

static uint16_t audio_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf, uint16_t max_len) {
    TU_VERIFY(desc_itf->bInterfaceClass == TUSB_CLASS_AUDIO && desc_itf->bInterfaceSubClass == 0x01, 0);
    // Take the whole function: everything up to the next interface that isn't ours.
    uint8_t const *p = (uint8_t const *)desc_itf, *end = p + max_len;
    uint8_t const itf = desc_itf->bInterfaceNumber;
    uint16_t len = 0;
    while (p + len < end) {
        uint8_t const *d = p + len;
        if (tu_desc_type(d) == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (tu_desc_type(d) == TUSB_DESC_INTERFACE) {
            uint8_t n = ((tusb_desc_interface_t const *)d)->bInterfaceNumber;
            if (n != itf && n != itf + 1) break;
        }
        if (tu_desc_type(d) == TUSB_DESC_ENDPOINT) s_audio.ep_desc = (tusb_desc_endpoint_t const *)d;
        len = (uint16_t)(len + tu_desc_len(d));
    }
    TU_ASSERT(s_audio.ep_desc, 0);
    s_audio.itf_ac = itf;
    s_audio.ep_out = s_audio.ep_desc->bEndpointAddress;
    s_audio.alt = 0;
    s_audio.running = false;   // allocating resets the endpoint
    TU_ASSERT(usbd_edpt_iso_alloc(rhport, s_audio.ep_out, EP_ALLOC), 0);
    return len;
}

static bool set_alt(uint8_t rhport, uint8_t alt) {
    s_audio.alt = alt;
    // Once enabled, the endpoint stays armed through alternate 0 (the host
    // sends nothing then): set it up the first time only.
    if (alt == 1 && !s_audio.running) {
        TU_ASSERT(usbd_edpt_iso_activate(rhport, s_audio.ep_desc));
        uint8_t num = tu_edpt_number(s_audio.ep_out);
        io_rw_32 *ep_ctrl = &usb_dpram->ep_ctrl[num - 1].out;
        // Double buffered, and no buffer interrupt (see the top).
        *ep_ctrl = (*ep_ctrl & ~(EP_CTRL_INTERRUPT_PER_BUFFER | EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER)) |
                   EP_CTRL_DOUBLE_BUFFERED_BITS;
        s_audio.buf = (const volatile uint32_t *)((uintptr_t)usb_dpram + (*ep_ctrl & 0xffc0u));
        s_audio.buf_ctrl = &usb_dpram->ep_buf_ctrl[num].out;
        arm_both();
        s_audio.running = true;
        usbd_sof_enable(rhport, SOF_CONSUMER_AUDIO, true);
    }
    headset_audio_usb_stream(alt == 1);
    return true;
}

static bool audio_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    uint8_t const type = request->bmRequestType_bit.type;
    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE && type == TUSB_REQ_TYPE_STANDARD) {
        uint8_t const itf = tu_u16_low(request->wIndex);
        switch (request->bRequest) {
        case TUSB_REQ_SET_INTERFACE:
            if (stage != CONTROL_STAGE_SETUP) return true;
            if (itf == s_audio.itf_ac + 1) {
                TU_VERIFY(request->wValue <= 1);
                TU_VERIFY(set_alt(rhport, (uint8_t)request->wValue));
            } else {
                TU_VERIFY(request->wValue == 0);
            }
            return tud_control_status(rhport, request);
        case TUSB_REQ_GET_INTERFACE:
            if (stage != CONTROL_STAGE_SETUP) return true;
            s_ctrl[0] = itf == s_audio.itf_ac + 1 ? s_audio.alt : 0;
            return tud_control_xfer(rhport, request, s_ctrl, 1);
        default:
            return false;
        }
    }
    // Class requests: the only control is the endpoint's sampling rate, which
    // the descriptor doesn't offer; some hosts set it anyway. Accept a SET,
    // answer a GET with 48 kHz.
    if (type != TUSB_REQ_TYPE_CLASS || request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_ENDPOINT) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    uint16_t n = tu_min16(request->wLength, 3);
    if (request->bmRequestType_bit.direction == TUSB_DIR_OUT) return tud_control_xfer(rhport, request, s_ctrl, n);
    s_ctrl[0] = (uint8_t)RATE;
    s_ctrl[1] = (uint8_t)(RATE >> 8);
    s_ctrl[2] = (uint8_t)(RATE >> 16);
    return tud_control_xfer(rhport, request, s_ctrl, n);
}

static bool audio_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    return true;   // the endpoint is handled in audio_sof()
}

const usbd_class_driver_t usb_audio_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "AUDIO",
#else
    .name = NULL,
#endif
    .init = audio_init,
    .deinit = audio_deinit,
    .reset = audio_reset,
    .open = audio_open,
    .control_xfer_cb = audio_control_xfer_cb,
    .xfer_cb = audio_xfer_cb,
    .sof = audio_sof,
};
