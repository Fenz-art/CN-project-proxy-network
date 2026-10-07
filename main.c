#define _POSIX_C_SOURCE 200809L
#include "access_control.h"
#include "cache.h"
#include "metrics.h"
#include "proxy_server.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logger.h"

typedef struct {
    int port;
    size_t cache_entries;
    size_t cache_max_response;
    unsigned cache_ttl;
    const char *access_file;
} options_t;

static int parse_size(const char *text, size_t minimum, size_t maximum,
                      size_t *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --port PORT                 Listen port (default 8080)\n"
           "  --cache-size ENTRIES        Cache capacity; 0 disables caching (default 32)\n"
           "  --cache-max-response BYTES  Maximum cached response (default 1048576)\n"
           "  --cache-ttl SECONDS         Cache lifetime (default 30)\n"
           "  --access-control FILE       Host allow/block rules\n"
           "  --help                      Show this help\n", program);
}

static int parse_options(int argc, char **argv, options_t *options)
{
    *options = (options_t){
        .port = 8080,
        .cache_entries = 32,
        .cache_max_response = 1024 * 1024,
        .cache_ttl = 30,
        .access_file = NULL
    };
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (i + 1 >= argc)
            return -1;
        const char *value = argv[++i];
        size_t parsed;
        if (strcmp(argv[i - 1], "--port") == 0) {
            if (parse_size(value, 1, 65535, &parsed) != 0)
                return -1;
            options->port = (int)parsed;
        } else if (strcmp(argv[i - 1], "--cache-size") == 0) {
            if (parse_size(value, 0, 100000, &options->cache_entries) != 0)
                return -1;
        } else if (strcmp(argv[i - 1], "--cache-max-response") == 0) {
            if (parse_size(value, 1, 64 * 1024 * 1024, &options->cache_max_response) != 0)
                return -1;
        } else if (strcmp(argv[i - 1], "--cache-ttl") == 0) {
            if (parse_size(value, 1, UINT_MAX, &parsed) != 0)
                return -1;
            options->cache_ttl = (unsigned)parsed;
        } else if (strcmp(argv[i - 1], "--access-control") == 0) {
            options->access_file = value;
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    options_t options;
    int parsed = parse_options(argc, argv, &options);
    if (parsed == 1)
        return 0;
    if (parsed != 0) {
        usage(argv[0]);
        return 2;
    }

    cache_t *cache = options.cache_entries == 0 ? NULL :
                     cache_create(options.cache_entries,
                                  options.cache_max_response,
                                  options.cache_ttl);
    access_control_t *access = access_control_create(1);
    Metrics metrics;
    if ((options.cache_entries != 0 && cache == NULL) || access == NULL) {
        fprintf(stderr, "Unable to initialize proxy modules\n");
        cache_destroy(cache);
        access_control_destroy(access);
        return 1;
    }
    if (options.access_file != NULL &&
        access_control_load_file(access, options.access_file) != 0) {
        fprintf(stderr, "Invalid/unreadable access-control file: %s\n",
                options.access_file);
        access_control_destroy(access);
        cache_destroy(cache);
        return 2;
    }
    init_metrics(&metrics);
    init_logger();
    proxy_server_config_t config = {
        .listen_port = options.port,
        .max_request_header = BUFFER_SIZE - 1,
        .max_cached_response = options.cache_max_response,
        .max_workers = 64,
        .cache = cache,
        .access_control = access,
        .metrics = &metrics
    };
    fprintf(stderr, "HTTP proxy listening on port %d\n", options.port);
    int result = proxy_server_run(&config);
    if (result != 0)
        perror("Proxy server stopped with an error");
    print_metrics(&metrics);
    save_metrics(&metrics);
    close_logger();
    destroy_metrics(&metrics);
    access_control_destroy(access);
    cache_destroy(cache);
    return result == 0 ? 0 : 1;
}