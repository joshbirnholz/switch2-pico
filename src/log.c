#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#define LOG_BUF_SIZE 8192

static char s_buf[LOG_BUF_SIZE];
static size_t s_head;      // next write position
static uint32_t s_total;   // total bytes ever written

void log_init(void) {
    s_head = 0;
    s_total = 0;
}

static void log_put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s_buf[s_head] = s[i];
        s_head = (s_head + 1) % LOG_BUF_SIZE;
    }
    s_total += (uint32_t)n;
}

void log_printf(const char *fmt, ...) {
    char line[192];
    uint32_t ms = to_ms_since_boot(get_absolute_time());
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
    fputs(line, stdout);
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
