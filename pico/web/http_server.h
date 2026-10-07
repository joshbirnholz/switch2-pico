#ifndef S2P_HTTP_SERVER_H
#define S2P_HTTP_SERVER_H

#include "web_api.h"

// Minimal HTTP/1.0 server on lwIP's raw TCP API (one request per connection).
void http_server_start(void);
void http_server_stop(void);

#endif
