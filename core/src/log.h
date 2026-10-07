#ifndef S2P_LOG_H
#define S2P_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Small in-RAM ring buffer log. Every line is also printed to stdio (the debug
// UART on GP0/GP1) and can be downloaded from the configuration web page, which
// makes it possible to debug the Bluetooth side without a serial adapter.

void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hex(const char *prefix, const uint8_t *data, size_t len);

// Copy the log out, oldest first. Returns the number of bytes written (no NUL).
size_t log_copy(char *dst, size_t dst_len);
// Monotonic count of bytes ever written; lets clients poll for new data.
uint32_t log_total_written(void);

#define LOG(...) log_printf(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
