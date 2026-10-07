#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"

#define LOG_BUF_SIZE 8192
#define LOG_MAGIC 0x4C4F4721u   // "LOG!"

// Kept out of the zero-initialised RAM where the board allows it, so the log
// from before a watchdog or crash reset can still be read after the reboot.
#if defined(S2P_LOG_NOINIT) || defined(ARDUINO_ARCH_NRF52)
#define LOG_KEEP __attribute__((section(".noinit")))
#else
#define LOG_KEEP
#endif

// Boards without a saved log.
__attribute__((weak)) void platform_log_flush(void) {}
__attribute__((weak)) size_t platform_saved_log(char *dst, size_t cap) {
    (void)dst;
    (void)cap;
    return 0;
}

LOG_KEEP static char s_buf[LOG_BUF_SIZE];
LOG_KEEP static size_t s_head;      // next write position
LOG_KEEP static uint32_t s_total;   // total bytes ever written
LOG_KEEP static uint32_t s_magic;
LOG_KEEP static uint32_t s_check;

void log_init(void) {
    if (s_magic == LOG_MAGIC && s_head < LOG_BUF_SIZE && s_check == (s_head ^ s_total ^ LOG_MAGIC)) {
        static const char mark[] = "----- reboot (earlier log kept above) -----\n";
        for (const char *p = mark; *p; p++) {
            s_buf[s_head] = *p;
            s_head = (s_head + 1) % LOG_BUF_SIZE;
            s_total++;
        }
        s_check = s_head ^ s_total ^ LOG_MAGIC;
        return;
    }
    s_head = 0;
    s_total = 0;
    s_magic = LOG_MAGIC;
    s_check = s_head ^ s_total ^ LOG_MAGIC;
}

static void log_put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s_buf[s_head] = s[i];
        s_head = (s_head + 1) % LOG_BUF_SIZE;
    }
    s_total += (uint32_t)n;
    s_check = s_head ^ s_total ^ LOG_MAGIC;
}

void log_printf(const char *fmt, ...) {
    char line[192];
    uint32_t ms = platform_millis();
    int n = snprintf(line, sizeof line, "[%7lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof line - 2) len = sizeof line - 2;
    line[len++] = '\n';
    line[len] = 0;
    log_put(line, len);
    platform_log_output(line);
}

void log_hex(const char *prefix, const uint8_t *data, size_t len) {
    char line[192];
    size_t pos = (size_t)snprintf(line, sizeof line, "%s (%u):", prefix, (unsigned)len);
    for (size_t i = 0; i < len && pos + 4 < sizeof line; i++) {
        pos += (size_t)snprintf(line + pos, sizeof line - pos, " %02x", data[i]);
    }
    log_printf("%s", line);
}

size_t log_copy(char *dst, size_t dst_len) {
    size_t avail = s_total < LOG_BUF_SIZE ? s_total : LOG_BUF_SIZE;
    if (avail > dst_len) avail = dst_len;
    size_t start = (s_head + LOG_BUF_SIZE - avail) % LOG_BUF_SIZE;
    for (size_t i = 0; i < avail; i++) {
        dst[i] = s_buf[(start + i) % LOG_BUF_SIZE];
    }
    return avail;
}

uint32_t log_total_written(void) {
    return s_total;
}
