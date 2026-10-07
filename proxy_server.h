#ifndef PROXY_SERVER_H
#define PROXY_SERVER_H

#include <stddef.h>

#include "access_control.h"
#include "cache.h"
#include "metrics.h"

typedef struct {
    int listen_port;
    size_t max_request_header;
    size_t max_cached_response;
    size_t max_workers;
    cache_t *cache;
    access_control_t *access_control;
    Metrics *metrics;
} proxy_server_config_t;

/* Blocks until SIGINT/SIGTERM or a listener error; returns 0 on shutdown. */
int proxy_server_run(const proxy_server_config_t *config);

#endif