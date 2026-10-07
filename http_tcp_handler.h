#ifndef HTTP_TCP_HANDLER_H
#define HTTP_TCP_HANDLER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#define BUFFER_SIZE 8192
#define ORIGIN_CONNECT_TIMEOUT_MS 5000

typedef struct {
    char method[16];
    char host[256];
    int port;
    char path[1024];
    char raw_request[BUFFER_SIZE];
} http_request_t;

/* Supports HTTP/1.0 and HTTP/1.1 method-token request lines with either
 * http:// absolute-form or slash-prefixed origin-form targets. Host is
 * required for origin-form and HTTP/1.1; IPv6 literals and HTTPS are rejected.
 * Input must be a NUL-terminated request no longer than BUFFER_SIZE - 1 bytes.
 */
int create_server_socket(int port);
int parse_http_request(const char *buffer, http_request_t *req);
/* The default timeout bounds TCP connection establishment; name resolution
 * uses the system resolver and is not separately timed by this API.
 */
int connect_to_origin(const char *host, int port);
int connect_to_origin_with_timeout(const char *host, int port, int timeout_ms);
/* Returns 0 at clean source EOF, or -1 with errno on receive/send failure. */
int relay_data(int src_fd, int dest_fd);

#endif