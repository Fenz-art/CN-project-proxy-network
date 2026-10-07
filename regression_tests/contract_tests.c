#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http_tcp_handler.h"
#include "logger.h"
#include "metrics.h"

#define WORKERS 12
#define METRIC_ITERATIONS 500000L
#define LOG_THREADS 20
#define LOG_ITERATIONS 1000

static Metrics metrics;
static pthread_barrier_t metric_barrier;
static int failures;
static int worker_success;
static int worker_cache_hit;
static int worker_mixed;

static void check(int condition, const char *name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

static int parse_is_rejected(const char *request) {
    http_request_t parsed;
    return parse_http_request(request, &parsed) != 0;
}

static void parser_tests(void) {
    http_request_t parsed;
    const char *valid_absolute =
        "GET http://example.com/index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
    check(parse_http_request(valid_absolute, &parsed) == 0 &&
          strcmp(parsed.host, "example.com") == 0 && parsed.port == 80 &&
          strcmp(parsed.path, "/index.html") == 0,
          "valid absolute-form request");
    check(parse_http_request("GET http://example.com:8080/index.html?a=1&b=2 HTTP/1.1\r\nHost: example.com:8080\r\n\r\n",
                             &parsed) == 0 && parsed.port == 8080 &&
          strcmp(parsed.path, "/index.html?a=1&b=2") == 0,
          "absolute-form explicit port and query");
    check(parse_is_rejected("GET / HTTP/1.1\r\n\r\n"), "missing Host rejected");
    check(parse_is_rejected("GET /index.html\r\nHost: example.com\r\n\r\n"),
          "missing HTTP version rejected");
    check(parse_is_rejected("GET http://example.com:0/x HTTP/1.1\r\n\r\n"),
          "port zero rejected");
    check(parse_is_rejected("GET http://example.com:65536/x HTTP/1.1\r\n\r\n"),
          "port above 65535 rejected");
    check(parse_is_rejected("GET http://example.com:abc/x HTTP/1.1\r\n\r\n"),
          "nonnumeric port rejected");
    check(parse_is_rejected("GET http://example.com:80tail/x HTTP/1.1\r\n\r\n"),
          "numeric port suffix rejected");
        check(parse_is_rejected("GET http://:8080/x HTTP/1.1\r\n\r\n"),
            "empty absolute hostname rejected");
        check(parse_is_rejected("GET http://[::1]:8080/x HTTP/1.1\r\n\r\n"),
            "unsupported IPv6 authority rejected cleanly");
        check(parse_is_rejected("GET http://user@example.com/x HTTP/1.1\r\n\r\n"),
            "userinfo authority rejected");
        check(parse_is_rejected("GET http://example.com/x#fragment HTTP/1.1\r\n\r\n"),
            "fragment in request target rejected");
        check(parse_is_rejected("GET * HTTP/1.1\r\nHost: example.com\r\n\r\n"),
            "unsupported asterisk-form rejected");
    check(parse_http_request("GET /x HTTP/1.1\r\nhost: example.com\r\n\r\n",
                             &parsed) == 0 && strcmp(parsed.host, "example.com") == 0,
          "lowercase Host recognized");
        check(parse_http_request("GET http://example.com/search?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n",
                             &parsed) == 0 && strcmp(parsed.path, "/search?q=1") == 0,
          "absolute-form query retained in path");
        check(parse_http_request("GET http://example.com?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n",
                         &parsed) == 0 && strcmp(parsed.path, "/?q=1") == 0,
            "absolute-form query without path retained");
        check(parse_http_request("GET /x HTTP/1.1\r\nHOST:\texample.com:8080 \r\n\r\n",
                         &parsed) == 0 && strcmp(parsed.host, "example.com") == 0 &&
            parsed.port == 8080,
            "origin-form Host case, whitespace, and port");
        check(parse_http_request("GET /x HTTP/1.1\r\nHost: example.com\r\n\r\n",
                                 &parsed) == 0 && strcmp(parsed.host, "example.com") == 0,
              "canonical Host header accepted");
        check(parse_http_request(NULL, &parsed) != 0, "NULL request buffer rejected");
            check(parse_http_request("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n", NULL) != 0,
                "NULL request structure rejected");
        check(parse_http_request("GET / HTTP/1.1\r\nHost:\r\n\r\n", &parsed) != 0,
            "empty Host rejected");
        check(parse_http_request("GET https://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n",
                         &parsed) != 0,
            "unsupported HTTPS request target rejected");
        check(parse_http_request("GET http:///x HTTP/1.1\r\nHost: example.com\r\n\r\n",
                         &parsed) != 0,
            "empty absolute authority rejected");
        char overlong_method[64];
        memset(overlong_method, 'G', 20);
        strcpy(overlong_method + 20, " / HTTP/1.1\r\nHost: example.com\r\n\r\n");
        check(parse_http_request(overlong_method, &parsed) != 0,
            "overlong method rejected");
        char overlong_host[301];
        memset(overlong_host, 'a', sizeof(overlong_host) - 1);
        overlong_host[sizeof(overlong_host) - 1] = '\0';
        char host_request[400];
        snprintf(host_request, sizeof(host_request),
             "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", overlong_host);
        check(parse_http_request(host_request, &parsed) != 0,
            "overlong host rejected");
        char overlong_path[1200];
        memset(overlong_path, 'p', sizeof(overlong_path) - 1);
        overlong_path[0] = '/';
        overlong_path[sizeof(overlong_path) - 1] = '\0';
        char path_request[1400];
        snprintf(path_request, sizeof(path_request),
             "GET %s HTTP/1.1\r\nHost: example.com\r\n\r\n", overlong_path);
        check(parse_http_request(path_request, &parsed) != 0,
            "overlong path rejected");
}

static void *accept_one(void *arg) {
    int listener = *(int *)arg;
    int client = accept(listener, NULL, NULL);
    if (client >= 0) close(client);
    close(listener);
    return (void *)(long)(client >= 0);
}

static void *payload_writer(void *arg) {
    int fd = *(int *)arg;
    char payload[8192];
    memset(payload, 'R', sizeof(payload));
    size_t remaining = 1024 * 1024;
    while (remaining > 0) {
        size_t requested = remaining < sizeof(payload) ? remaining : sizeof(payload);
    #ifdef MSG_NOSIGNAL
        ssize_t written = send(fd, payload, requested, MSG_NOSIGNAL);
    #else
        ssize_t written = send(fd, payload, requested, 0);
    #endif
        if (written <= 0) break;
        remaining -= (size_t)written;
    }
    shutdown(fd, SHUT_WR);
    return (void *)(long)(remaining == 0);
}

static void *payload_reader(void *arg) {
    int fd = *(int *)arg;
    char payload[8192];
    size_t received_total = 0;
    ssize_t received;
    while ((received = recv(fd, payload, sizeof(payload), 0)) > 0) {
        for (ssize_t i = 0; i < received; ++i) {
            if (payload[i] != 'R')
                return (void *)0;
        }
        received_total += (size_t)received;
    }
    return (void *)(long)(received_total == 1024 * 1024);
}

static void *close_after_payload(void *arg) {
    int fd = *(int *)arg;
    char byte;
    recv(fd, &byte, sizeof(byte), 0);
    close(fd);
    return NULL;
}

static int make_listener(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 2) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &size) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static void transport_tests(void) {
    int server_fd = create_server_socket(0);
    struct sockaddr_in bound_address;
    socklen_t bound_size = sizeof(bound_address);
    int listener_connected = 0;
    if (server_fd >= 0 &&
        getsockname(server_fd, (struct sockaddr *)&bound_address, &bound_size) == 0) {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd >= 0 &&
            connect(client_fd, (struct sockaddr *)&bound_address,
                    sizeof(bound_address)) == 0)
            listener_connected = 1;
        if (client_fd >= 0) close(client_fd);
    }
        check(listener_connected &&
            bound_address.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
            "create_server_socket listens only on loopback by default");
    if (server_fd >= 0) close(server_fd);

    int port;
    int listener = make_listener(&port);
    pthread_t accept_thread;
    pthread_create(&accept_thread, NULL, accept_one, &listener);
    int origin = connect_to_origin("127.0.0.1", port);
    void *accepted;
    pthread_join(accept_thread, &accepted);
    check(origin >= 0 && (long)accepted == 1, "connect to local origin");
    if (origin >= 0) close(origin);

        listener = make_listener(&port);
        pthread_create(&accept_thread, NULL, accept_one, &listener);
        origin = connect_to_origin_with_timeout("127.0.0.1", port, 1000);
        pthread_join(accept_thread, &accepted);
        check(origin >= 0 && (long)accepted == 1,
            "timeout-configured origin connection succeeds locally");
        if (origin >= 0) close(origin);
        errno = 0;
        check(connect_to_origin_with_timeout("127.0.0.1", port, -1) == -1 && errno == EINVAL,
            "negative connection timeout rejected");
        check(connect_to_origin_with_timeout("", 80, 100) == -1 && errno == EINVAL,
            "empty origin host rejected");
            check(connect_to_origin_with_timeout("bad host", 80, 100) == -1,
                "invalid origin hostname fails cleanly");

    listener = make_listener(&port);
    close(listener);
    check(connect_to_origin("127.0.0.1", port) == -1,
          "closed local port reports failure");

    int source[2], destination[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, source);
    socketpair(AF_UNIX, SOCK_STREAM, 0, destination);
    send(source[1], "relay-check", 11, 0);
    shutdown(source[1], SHUT_WR);
        check(relay_data(source[0], destination[0]) == 0,
            "relay reports clean source EOF");
    char response[16] = {0};
    ssize_t count = recv(destination[1], response, sizeof(response), 0);
    check(count == 11 && memcmp(response, "relay-check", 11) == 0,
          "relay forwards a small complete payload");
    close(source[0]); close(source[1]);
    close(destination[0]); close(destination[1]);

    socketpair(AF_UNIX, SOCK_STREAM, 0, source);
    socketpair(AF_UNIX, SOCK_STREAM, 0, destination);
    int send_buffer = 1024;
    setsockopt(destination[0], SOL_SOCKET, SO_SNDBUF,
               &send_buffer, sizeof(send_buffer));
    pthread_t writer, reader;
    pthread_create(&reader, NULL, payload_reader, &destination[1]);
    pthread_create(&writer, NULL, payload_writer, &source[1]);
    relay_data(source[0], destination[0]);
    pthread_join(writer, NULL);
    shutdown(destination[0], SHUT_WR);
    void *payload_ok;
    pthread_join(reader, &payload_ok);
    check((long)payload_ok == 1, "relay preserves all bytes under backpressure");
    close(source[0]); close(source[1]);
    close(destination[0]); close(destination[1]);

        socketpair(AF_UNIX, SOCK_STREAM, 0, source);
        socketpair(AF_UNIX, SOCK_STREAM, 0, destination);
        close(destination[1]);
        send(source[1], "x", 1, 0);
        shutdown(source[1], SHUT_WR);
        errno = 0;
        check(relay_data(source[0], destination[0]) == -1 && errno == EPIPE,
            "closed destination returns EPIPE without SIGPIPE termination");
        close(source[0]); close(source[1]); close(destination[0]);

            socketpair(AF_UNIX, SOCK_STREAM, 0, source);
            socketpair(AF_UNIX, SOCK_STREAM, 0, destination);
            pthread_t closer, producer;
            pthread_create(&closer, NULL, close_after_payload, &destination[1]);
            pthread_create(&producer, NULL, payload_writer, &source[1]);
            errno = 0;
            int relay_result = relay_data(source[0], destination[0]);
            int relay_errno = errno;
            close(source[0]);
            pthread_join(producer, NULL);
            pthread_join(closer, NULL);
            check(relay_result == -1 &&
                (relay_errno == EPIPE || relay_errno == ECONNRESET),
                "destination disconnect during transfer reports failure");
            close(source[1]); close(destination[0]);

        errno = 0;
        check(relay_data(-1, 1) == -1 && errno == EBADF,
            "relay reports receive failure");
}

static void *metric_worker(void *unused) {
    (void)unused;
    pthread_barrier_wait(&metric_barrier);
    for (long i = 0; i < METRIC_ITERATIONS; ++i) {
        int success = worker_mixed ? (int)(i % 2) : worker_success;
        int cache_hit = worker_mixed ? (int)((i / 2) % 2) : worker_cache_hit;
        record_request(&metrics, success, cache_hit, 64, 1.5);
    }
    return NULL;
}

static void *log_worker(void *unused) {
    (void)unused;
    for (int i = 0; i < LOG_ITERATIONS; ++i)
        log_request("127.0.0.1", "GET", "local.test", "/", 200, 0, 32, 0.5);
    return NULL;
}

static int run_metric_mix(int success, int cache_hit, const char *name) {
    pthread_t threads[WORKERS];
    worker_success = success;
    worker_cache_hit = cache_hit;
    worker_mixed = 0;
    init_metrics(&metrics);
    pthread_barrier_init(&metric_barrier, NULL, WORKERS);
    for (int i = 0; i < WORKERS; ++i)
        pthread_create(&threads[i], NULL, metric_worker, NULL);
    for (int i = 0; i < WORKERS; ++i)
        pthread_join(threads[i], NULL);
    long expected = WORKERS * METRIC_ITERATIONS;
    int valid = metrics.total_requests == expected &&
                metrics.successful_requests == (success ? expected : 0) &&
                metrics.failed_requests == (success ? 0 : expected) &&
                metrics.cache_hits == (cache_hit ? expected : 0) &&
                metrics.cache_misses == (cache_hit ? 0 : expected) &&
                metrics.successful_requests + metrics.failed_requests ==
                    metrics.total_requests &&
                metrics.cache_hits + metrics.cache_misses == metrics.total_requests &&
                metrics.total_bytes == expected * 64 &&
                metrics.total_latency == expected * 1.5;
    check(valid, name);
    pthread_barrier_destroy(&metric_barrier);
    destroy_metrics(&metrics);
    return valid;
}

static void run_mixed_metrics(void) {
    pthread_t threads[WORKERS];
    worker_mixed = 1;
    init_metrics(&metrics);
    pthread_barrier_init(&metric_barrier, NULL, WORKERS);
    for (int i = 0; i < WORKERS; ++i)
        pthread_create(&threads[i], NULL, metric_worker, NULL);
    for (int i = 0; i < WORKERS; ++i)
        pthread_join(threads[i], NULL);
    long expected = WORKERS * METRIC_ITERATIONS;
    check(metrics.total_requests == expected &&
          metrics.successful_requests == expected / 2 &&
          metrics.failed_requests == expected / 2 &&
          metrics.cache_hits == expected / 2 &&
          metrics.cache_misses == expected / 2 &&
          metrics.successful_requests + metrics.failed_requests == expected &&
          metrics.cache_hits + metrics.cache_misses == expected &&
          metrics.total_bytes == expected * 64 &&
          metrics.total_latency == expected * 1.5,
          "12x500k mixed success/failure and hit/miss invariants exact");
    pthread_barrier_destroy(&metric_barrier);
    destroy_metrics(&metrics);
    worker_mixed = 0;
}

static void metric_logger_tests(void) {
    init_metrics(&metrics);
    print_metrics(&metrics);
    check(metrics.total_requests == 0 && metrics.total_bytes == 0,
          "zero-request metrics state");
    destroy_metrics(&metrics);
    run_metric_mix(1, 1, "12x500k successful requests and cache hits exact");
    run_metric_mix(0, 0, "12x500k failed requests and cache misses exact");
    run_metric_mix(1, 0, "12x500k mixed outcome invariants exact");
    run_mixed_metrics();

    init_logger();
    pthread_t log_threads[LOG_THREADS];
    for (int i = 0; i < LOG_THREADS; ++i)
        pthread_create(&log_threads[i], NULL, log_worker, NULL);
    for (int i = 0; i < LOG_THREADS; ++i)
        pthread_join(log_threads[i], NULL);
    close_logger();
    FILE *log = fopen("proxy.log", "r");
    long lines = 0;
        int malformed = 0;
    char *line = NULL;
    size_t capacity = 0;
        while (log && getline(&line, &capacity, log) >= 0) {
          int day, month, year, hour, minute, second;
          ++lines;
          if (sscanf(line, "[%2d-%2d-%4d %2d:%2d:%2d]", &day, &month, &year,
                 &hour, &minute, &second) != 6 ||
            day < 1 || day > 31 || month < 1 || month > 12 ||
            year < 2020 || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 || second < 0 || second > 60 ||
            strchr(line, '\n') == NULL ||
            strstr(line, "| Method=GET | Host=local.test | URL=/") == NULL)
            ++malformed;
        }
    if (log) fclose(log);
    free(line);
        check(lines == LOG_THREADS * LOG_ITERATIONS && malformed == 0,
            "concurrent logger retains valid, complete, uncorrupted records");
}

int main(void) {
    parser_tests();
    transport_tests();
    metric_logger_tests();
    printf("Contract test failures: %d\n", failures);
    return failures ? 1 : 0;
}