#ifndef S2P_WEB_API_H
#define S2P_WEB_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Request/response types shared by the HTTP server (Pico Wi-Fi) and the
// WebUSB channel; both dispatch to web_api_handle().

typedef struct {
    int status;                 // 200, 302, 404 ...
    const char *content_type;
    const uint8_t *body;
    size_t body_len;
    bool body_owned;            // free(body) when done
    const char *extra_headers;  // optional, each line terminated by \r\n
} http_response_t;

typedef struct {
    const char *method;
    const char *path;           // without the query string
    const char *query;          // may be empty
    const char *body;           // NUL terminated
    size_t body_len;
} http_request_t;

void web_api_handle(const http_request_t *req, http_response_t *resp);

#endif
