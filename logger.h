#ifndef LOGGER_H
#define LOGGER_H

void init_logger();

void log_request(
    const char *client_ip,
    const char *method,
    const char *host,
    const char *url,
    int status_code,
    int cache_hit,
    long response_size,
    double latency
);

void close_logger();

#endif
