#ifndef S2P_HTTP_SERVER_H
#define S2P_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal HTTP/1.0 server on lwIP's raw TCP API (one request per connection).

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

void http_server_start(void);
void http_server_stop(void);

// Implemented by web_api.c.
void web_api_handle(const http_request_t *req, http_response_t *resp);

#endif
