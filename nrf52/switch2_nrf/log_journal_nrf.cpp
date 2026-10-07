// Saved log for nRF52840 boards: every log line is also appended to a small
// ring of flash pages, so the log from before a reset or a power loss can be
// read after the next start (the RAM log can't survive those: the UF2
// bootloader reuses that RAM, and power loss clears it).
//
// Flash map (see fw_update_nrf.cpp): the update staging area ends at
// 0xE6000 and its record is at 0xEC000; the journal uses the four free pages
// 0xE8000-0xEBFFF. Each page starts with a sequence number (0xFFFFFFFF:
// unused); log text follows, padded with NUL bytes to whole words, so the
// first all-0xFF word marks the end. Pages are erased only when the ring
// moves on, about once per 4 KB of log.
//
// Lines are collected in RAM and written about once a second (sooner when a
// lot is pending) with short SoftDevice flash writes. The SoftDevice reports
// completion through the flash library's event callback; it is wrapped
// (-Wl,--wrap=flash_nrf5x_event_cb, see nrf52/build.sh) so events for the
// journal's own writes don't reach InternalFS.

#include <Arduino.h>
#include <string.h>

#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "platform.h"

#define J_BASE     0xE8000UL
#define J_PAGES    4
#define J_PAGE     4096UL
#define J_FREE     0xFFFFFFFFUL
#define J_PENDING  2048
#define J_CHUNK    256
#define J_FLUSH_MS 1000

static char s_pend[J_PENDING];
static uint16_t s_pend_len;
static uint32_t s_pend_since;
static uint32_t s_dropped;

static int s_page = -1;        // page being written (-1: none yet)
static uint32_t s_seq;         // its sequence number
static uint32_t s_off;         // next write offset in it (bytes)
static int s_boot_page = -1;   // where this boot's lines start
static uint32_t s_boot_seq, s_boot_off;

static uint32_t s_wbuf[J_CHUNK / 4];
static volatile bool s_op_busy, s_op_done, s_op_ok;

extern "C" void __real_flash_nrf5x_event_cb(uint32_t event);
extern "C" void __wrap_flash_nrf5x_event_cb(uint32_t event) {
    if (s_op_busy) {
        s_op_ok = event == NRF_EVT_FLASH_OPERATION_SUCCESS;
        s_op_done = true;
        return;
    }
    __real_flash_nrf5x_event_cb(event);
}

static uint32_t page_addr(int p) {
    return J_BASE + (uint32_t)p * J_PAGE;
}

static uint32_t page_seq(int p) {
    return *(const volatile uint32_t *)page_addr(p);
}

// Offset of the first unused word in page p.
static uint32_t page_end(int p) {
    const volatile uint32_t *w = (const volatile uint32_t *)page_addr(p);
    uint32_t i = 1;
    while (i < J_PAGE / 4 && w[i] != J_FREE) i++;
    return i * 4;
}

// One SoftDevice flash operation, waited for (a few ms for a write, ~90 ms
// for an erase). False if the flash is busy or the operation failed.
static bool flash_op(bool erase, uint32_t addr, uint32_t words) {
    uint8_t sd = 0;
    sd_softdevice_is_enabled(&sd);
    if (!sd) return false;
    // No task switch between starting the operation and marking it ours, so
    // its completion event can't be passed on to InternalFS.
    vTaskSuspendAll();
    s_op_done = false;
    uint32_t err = erase ? sd_flash_page_erase(addr / J_PAGE) : sd_flash_write((uint32_t *)addr, s_wbuf, words);
    if (err == NRF_SUCCESS) s_op_busy = true;
    xTaskResumeAll();
    if (err != NRF_SUCCESS) return false;
    uint32_t t0 = millis();
    while (!s_op_done && millis() - t0 < 500) yield();
    s_op_busy = false;
    return s_op_done && s_op_ok;
}

static bool start_page(int p, uint32_t seq) {
    if (page_seq(p) != J_FREE && !flash_op(true, page_addr(p), 0)) return false;
    s_wbuf[0] = seq;
    if (!flash_op(false, page_addr(p), 1)) return false;
    s_page = p;
    s_seq = seq;
    s_off = 4;
    return true;
}

extern "C" void platform_journal_init(void) {
    uint32_t best = 0;
    for (int p = 0; p < J_PAGES; p++) {
        uint32_t q = page_seq(p);
        if (q != J_FREE && (s_page < 0 || q > best)) {
            best = q;
            s_page = p;
        }
    }
    if (s_page >= 0) {
        s_seq = best;
        s_off = page_end(s_page);
        s_boot_page = s_page;
        s_boot_seq = s_seq;
        s_boot_off = s_off;
    }
}

// Lines can come from the Bluetooth tasks too: the pending buffer is only
// touched in short critical sections.
extern "C" void platform_journal_append(const char *line, size_t len) {
    taskENTER_CRITICAL();
    if (s_pend_len + len > sizeof s_pend) {
        s_dropped++;
    } else {
        if (!s_pend_len) s_pend_since = millis();
        memcpy(s_pend + s_pend_len, line, len);
        s_pend_len = (uint16_t)(s_pend_len + len);
    }
    taskEXIT_CRITICAL();
}

extern "C" void platform_journal_flush(void) {
    static bool busy;   // not re-entered (a log line while flushing)
    if (busy) return;
    busy = true;
    while (s_pend_len) {
        if (s_page < 0 || s_off >= J_PAGE) {
            int next = s_page < 0 ? 0 : (s_page + 1) % J_PAGES;
            if (!start_page(next, s_page < 0 ? 1 : s_seq + 1)) break;   // try again later
        }
        memset(s_wbuf, 0, sizeof s_wbuf);
        taskENTER_CRITICAL();
        uint32_t n = s_pend_len;
        if (n > J_CHUNK) n = J_CHUNK;
        if (n > J_PAGE - s_off) n = J_PAGE - s_off;
        memcpy(s_wbuf, s_pend, n);
        taskEXIT_CRITICAL();
        uint32_t words = (n + 3) / 4;
        if (!flash_op(false, page_addr(s_page) + s_off, words)) break;
        s_off += words * 4;
        taskENTER_CRITICAL();
        memmove(s_pend, s_pend + n, s_pend_len - n);
        s_pend_len = (uint16_t)(s_pend_len - n);
        taskEXIT_CRITICAL();
    }
    s_pend_since = millis();
    busy = false;
}

extern "C" void platform_journal_task(void) {
    if (!s_pend_len) return;
    if (s_pend_len >= J_PENDING / 2 || millis() - s_pend_since >= J_FLUSH_MS) platform_journal_flush();
}

// Log text saved before this boot (oldest first), the newest `cap` bytes of it.
extern "C" size_t platform_saved_log(char *dst, size_t cap) {
    if (s_boot_page < 0 || page_seq(s_boot_page) != s_boot_seq) return 0;
    // Pages in sequence order, ending with the boot page (up to s_boot_off).
    int order[J_PAGES];
    int n = 0;
    for (uint32_t back = J_PAGES; back-- > 0;) {
        if (s_boot_seq < back + 1) continue;
        uint32_t want = s_boot_seq - back;
        for (int p = 0; p < J_PAGES; p++) {
            if (page_seq(p) == want) {
                order[n++] = p;
                break;
            }
        }
    }
    // Count the text first, then copy the newest `cap` bytes.
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t end = order[i] == s_boot_page ? s_boot_off : page_end(order[i]);
        const uint8_t *b = (const uint8_t *)page_addr(order[i]);
        for (uint32_t k = 4; k < end; k++) total += b[k] != 0 && b[k] != 0xFF;
    }
    size_t skip = total > cap ? total - cap : 0, out = 0;
    for (int i = 0; i < n; i++) {
        uint32_t end = order[i] == s_boot_page ? s_boot_off : page_end(order[i]);
        const uint8_t *b = (const uint8_t *)page_addr(order[i]);
        for (uint32_t k = 4; k < end; k++) {
            if (b[k] == 0 || b[k] == 0xFF) continue;
            if (skip) {
                skip--;
                continue;
            }
            dst[out++] = (char)b[k];
        }
    }
    return out;
}
