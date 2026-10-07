#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LARGE_SIZE (1024 * 1024)
#define RESPONSE_LIMIT (2 * 1024 * 1024)

typedef struct {
    unsigned char *data;
    size_t length;
    size_t header_length;
    int status;
} response_t;

static int proxy_port;
static int origin_port;
static int second_origin_port;
static int failures;

static void check(int ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

static int send_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, data + offset, length - offset, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static int unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    socklen_t size = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &size) != 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int read_response(int fd, response_t *r)
{
    size_t cap = 16384;
    size_t expected_body = 0;
    int has_content_length = 0;
    r->data = malloc(cap + 1);
    if (r->data == NULL) return -1;
    r->length = 0;
    r->header_length = 0;
    r->status = 0;
    for (;;) {
        if (r->length == cap) {
            if (cap >= RESPONSE_LIMIT) return -1;
            size_t next = cap * 2;
            unsigned char *grown = realloc(r->data, next + 1);
            if (grown == NULL) return -1;
            r->data = grown;
            cap = next;
        }
        ssize_t n = recv(fd, r->data + r->length, cap - r->length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        r->length += (size_t)n;
        r->data[r->length] = '\0';
        if (r->header_length == 0) {
            for (size_t i = 3; i < r->length; ++i) {
                if (memcmp(r->data + i - 3, "\r\n\r\n", 4) == 0) {
                    r->header_length = i + 1;
                    if (sscanf((char *)r->data, "HTTP/%*s %d", &r->status) != 1)
                        return -1;
                    char *transfer_encoding = strstr((char *)r->data,
                                                     "Transfer-Encoding:");
                    char *content_length = strstr((char *)r->data, "Content-Length:");
                    if (transfer_encoding == NULL && content_length != NULL && content_length <
                        (char *)r->data + r->header_length) {
                        expected_body = (size_t)strtoull(strchr(content_length, ':') + 1,
                                                        NULL, 10);
                        has_content_length = 1;
                    }
                    break;
                }
            }
        }
        if (has_content_length &&
            r->length >= r->header_length + expected_body)
            break;
    }
    return r->header_length ? 0 : -1;
}

static void free_response(response_t *r)
{
    free(r->data);
    memset(r, 0, sizeof(*r));
}

static int request_raw(const char *request, response_t *response)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)proxy_port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        send_all(fd, request, strlen(request)) != 0) {
        close(fd);
        return -1;
    }
    int result = read_response(fd, response);
    close(fd);
    return result;
}

static int request_proxy(const char *host, int target_port, const char *path,
                         int fragmented, response_t *response)
{
    char request[4096];
    int length = snprintf(request, sizeof(request),
        "GET http://%s:%d%s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        host, target_port, path, host, target_port);
    if (length < 0 || (size_t)length >= sizeof(request)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)proxy_port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    int sent = 0;
    if (fragmented) {
        size_t split = (size_t)length / 2;
        sent = send_all(fd, request, split);
        if (sent == 0) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&pause, NULL);
            sent = send_all(fd, request + split, (size_t)length - split);
        }
    } else {
        sent = send_all(fd, request, (size_t)length);
    }
    int result = sent == 0 ? read_response(fd, response) : -1;
    close(fd);
    return result;
}

static int response_body_equals(const response_t *r, const char *body)
{
    size_t n = strlen(body);
    return r->length >= r->header_length &&
           r->length - r->header_length == n &&
           memcmp(r->data + r->header_length, body, n) == 0;
}

static int response_contains(const response_t *r, const char *text)
{
    if (r->length == 0) return 0;
    unsigned char *copy = malloc(r->length + 1);
    if (copy == NULL) return 0;
    memcpy(copy, r->data, r->length);
    copy[r->length] = '\0';
    int found = strstr((char *)copy, text) != NULL;
    free(copy);
    return found;
}

static int fetch_count_at(int port, const char *path, unsigned long *value)
{
    char target[256];
    static unsigned long nonce;
    snprintf(target, sizeof(target), "/_count?path=%s&nonce=%lu", path, ++nonce);
    response_t r = {0};
    int result = request_proxy("127.0.0.1", port, target, 0, &r);
    if (result == 0 && r.status == 200 && r.length - r.header_length < 32) {
        char text[32] = {0};
        size_t n = r.length - r.header_length;
        memcpy(text, r.data + r.header_length, n);
        *value = strtoul(text, NULL, 10);
    } else {
        result = -1;
    }
    free_response(&r);
    return result;
}

static int fetch_count(const char *path, unsigned long *value)
{
    return fetch_count_at(origin_port, path, value);
}

static int start_origin(const char *fixture, int *port, pid_t *pid)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    *pid = fork();
    if (*pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl(fixture, fixture, "0", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (*pid < 0) { close(fds[0]); return -1; }
    FILE *stream = fdopen(fds[0], "r");
    char line[32];
    if (stream == NULL || fgets(line, sizeof(line), stream) == NULL) {
        if (stream != NULL) fclose(stream);
        kill(*pid, SIGTERM);
        waitpid(*pid, NULL, 0);
        return -1;
    }
    *port = atoi(line);
    fclose(stream);
    return *port > 0 ? 0 : -1;
}

static int wait_proxy(pid_t pid, int port)
{
    for (int i = 0; i < 500; ++i) {
        if (kill(pid, 0) != 0) return -1;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((uint16_t)port);
            int ready = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            close(fd);
            if (ready) return 0;
        }
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&pause, NULL);
    }
    return -1;
}

static pid_t start_proxy(const char *binary, const char *rules, int port,
                         int cache_entries)
{
    pid_t child = fork();
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) { dup2(null_fd, STDERR_FILENO); close(null_fd); }
        char port_text[16];
        char cache_text[16];
        snprintf(port_text, sizeof(port_text), "%d", port);
        snprintf(cache_text, sizeof(cache_text), "%d", cache_entries);
          execl(binary, binary, "--port", port_text, "--cache-size", cache_text,
              "--cache-max-response", "1048576", "--cache-ttl", "2",
              "--access-control", rules, (char *)NULL);
        _exit(127);
    }
    return child;
}

static int run_benchmark(char *const args[], char *output, size_t output_size)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execv(args[0], args);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return -1; }
    size_t used = 0;
    while (used + 1 < output_size) {
        ssize_t count = read(fds[0], output + used, output_size - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
    }
    output[used] = '\0';
    close(fds[0]);
    int status;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

static double find_measurement(const char *output, const char *prefix)
{
    const char *line = strstr(output, prefix);
    if (line == NULL) return -1.0;
    return strtod(strchr(line, ':') + 1, NULL);
}

static void run_real_benchmarks(const char *benchmark_path)
{
    const int levels[] = {1, 5, 10, 25, 50};
    const int repetitions = 3;
    char origin_port_text[16], proxy_port_text[16];
    snprintf(origin_port_text, sizeof(origin_port_text), "%d", origin_port);
    snprintf(proxy_port_text, sizeof(proxy_port_text), "%d", proxy_port);
    puts("REAL DIRECT versus REAL PROXY benchmark (cache disabled; local origin)");
    puts("clients repetitions direct_ms proxy_ms latency_overhead_pct direct_rps proxy_rps");
    for (size_t level = 0; level < sizeof(levels) / sizeof(levels[0]); ++level) {
        double direct_latency = 0.0, proxy_latency = 0.0;
        double direct_rate = 0.0, proxy_rate = 0.0;
        int valid = 1;
        char client_text[16];
        snprintf(client_text, sizeof(client_text), "%d", levels[level]);
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            char direct_output[4096], proxy_output[4096];
            char *direct_args[] = {
                (char *)benchmark_path, "direct", "127.0.0.1",
                origin_port_text, "/query?benchmark=shared", client_text, NULL
            };
            char *proxy_args[] = {
                (char *)benchmark_path, "proxy", "127.0.0.1", proxy_port_text,
                "127.0.0.1", origin_port_text, "/query?benchmark=shared",
                client_text, NULL
            };
            if (run_benchmark(direct_args, direct_output, sizeof(direct_output)) != 0 ||
                run_benchmark(proxy_args, proxy_output, sizeof(proxy_output)) != 0) {
                valid = 0;
                break;
            }
            double d_latency = find_measurement(direct_output, "Average latency");
            double p_latency = find_measurement(proxy_output, "Average latency");
            double d_rate = find_measurement(direct_output, "Throughput");
            double p_rate = find_measurement(proxy_output, "Throughput");
            if (d_latency < 0 || p_latency < 0 || d_rate < 0 || p_rate < 0) {
                valid = 0;
                break;
            }
            direct_latency += d_latency;
            proxy_latency += p_latency;
            direct_rate += d_rate;
            proxy_rate += p_rate;
        }
        if (!valid) {
            check(0, "real direct/proxy benchmark repetition");
            continue;
        }
        direct_latency /= repetitions;
        proxy_latency /= repetitions;
        direct_rate /= repetitions;
        proxy_rate /= repetitions;
        double overhead = direct_latency > 0.0
                              ? (proxy_latency - direct_latency) * 100.0 / direct_latency
                              : 0.0;
        printf("%d %d %.3f %.3f %.2f%% %.1f %.1f\n", levels[level],
               repetitions, direct_latency, proxy_latency, overhead,
               direct_rate, proxy_rate);
        char label[80];
        snprintf(label, sizeof(label), "real direct and proxy benchmarks at %d clients", levels[level]);
        check(1, label);
    }
}

typedef struct { int id; int ok; int status; int started; } request_arg_t;

static void *run_request(void *opaque)
{
    request_arg_t *arg = opaque;
    char path[128];
    snprintf(path, sizeof(path), "/query?client=%d", arg->id);
    response_t response = {0};
    char expected[160];
    snprintf(expected, sizeof(expected), "origin:%s\n", path);
        int result = request_proxy("127.0.0.1", origin_port, path, 0, &response);
        arg->status = response.status;
        arg->ok = result == 0 && response.status == 200 &&
                  response_body_equals(&response, expected);
    free_response(&response);
    return NULL;
}

static void run_concurrency_levels(void)
{
    const int levels[] = {1, 2, 5, 10, 25, 50};
    for (size_t level = 0; level < sizeof(levels) / sizeof(levels[0]); ++level) {
        int count = levels[level];
        pthread_t *threads = calloc((size_t)count, sizeof(*threads));
        request_arg_t *args = calloc((size_t)count, sizeof(*args));
        if (threads == NULL || args == NULL) { check(0, "allocate concurrent clients"); free(threads); free(args); return; }
        int created = 0;
        for (int i = 0; i < count; ++i) {
            args[i].id = count * 1000 + i;
            if (pthread_create(&threads[i], NULL, run_request, &args[i]) == 0) {
                args[i].started = 1;
                ++created;
            }
        }
        int ok = 0;
        for (int i = 0; i < count; ++i) {
            if (args[i].started) {
                pthread_join(threads[i], NULL);
                ok += args[i].ok;
            }
        }
        char label[80];
        snprintf(label, sizeof(label), "%d simultaneous client requests complete", count);
        check(created == count && ok == count, label);
        free(args); free(threads);
    }
}

static pthread_barrier_t overload_barrier;

static void *run_overload_request(void *opaque)
{
    request_arg_t *arg = opaque;
    pthread_barrier_wait(&overload_barrier);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)proxy_port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        send_all(fd, "GET / HTTP/1.1\r\nHost: localhost\r\n", 33) != 0) {
        close(fd);
        return NULL;
    }
    response_t response = {0};
    arg->ok = read_response(fd, &response) == 0;
    arg->status = response.status;
    free_response(&response);
    close(fd);
    return NULL;
}

static void test_worker_capacity(void)
{
    enum { CLIENT_COUNT = 65 };
    pthread_t threads[CLIENT_COUNT];
    request_arg_t args[CLIENT_COUNT] = {0};
    pthread_barrier_init(&overload_barrier, NULL, CLIENT_COUNT);
    int created = 0;
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        if (pthread_create(&threads[i], NULL, run_overload_request, &args[i]) == 0) {
            args[i].started = 1;
            ++created;
        }
    }
    int rejected = 0;
    int completed = 0;
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        if (!args[i].started) continue;
        pthread_join(threads[i], NULL);
        completed += args[i].ok;
        rejected += args[i].status == 503;
    }
    pthread_barrier_destroy(&overload_barrier);
    if (created != CLIENT_COUNT || completed != CLIENT_COUNT || rejected == 0)
        fprintf(stderr, "overload diagnostic created=%d completed=%d rejected503=%d\n",
                created, completed, rejected);
    check(created == CLIENT_COUNT && completed == CLIENT_COUNT && rejected >= 1,
          "65 simultaneous requests respect 64-worker cap with HTTP 503");
}

static void test_client_disconnect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { check(0, "create disconnecting client"); return; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)proxy_port);
    const char *request_format =
        "GET http://127.0.0.1:%d/large HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Connection: close\r\n\r\n";
    char request[256];
    int request_length = snprintf(request, sizeof(request), request_format,
                                  origin_port, origin_port);
    int started = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                  request_length > 0 &&
                  send_all(fd, request, (size_t)request_length) == 0;
    unsigned char discard[128];
    ssize_t received = started ? recv(fd, discard, sizeof(discard), 0) : -1;
    struct linger reset = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    close(fd);
    check(started && received > 0, "client disconnects during a large response");

    response_t response = {0};
    int result = request_proxy("127.0.0.1", origin_port, "/hello", 0, &response);
    check(result == 0 && response.status == 200,
          "proxy remains available after client disconnect");
    free_response(&response);
}

typedef struct {
    int fd;
    pthread_mutex_t lock;
    int stop;
} slow_header_arg_t;

static void *send_slow_header(void *opaque)
{
    slow_header_arg_t *arg = opaque;
    static const char request[] =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (size_t i = 0; i < sizeof(request) - 1; ++i) {
        pthread_mutex_lock(&arg->lock);
        int stop = arg->stop;
        pthread_mutex_unlock(&arg->lock);
        if (stop)
            break;
#ifdef MSG_NOSIGNAL
        if (send(arg->fd, request + i, 1, MSG_NOSIGNAL) != 1)
#else
        if (send(arg->fd, request + i, 1, 0) != 1)
#endif
            break;
        struct timespec remaining = { .tv_sec = 0, .tv_nsec = 500000000L };
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
    }
    return NULL;
}

static void test_client_header_deadline(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { check(0, "create slow-header client"); return; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)proxy_port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        check(0, "connect slow-header client");
        return;
    }
    slow_header_arg_t arg = { .fd = fd, .lock = PTHREAD_MUTEX_INITIALIZER, .stop = 0 };
    pthread_t sender;
    if (pthread_create(&sender, NULL, send_slow_header, &arg) != 0) {
        close(fd);
        pthread_mutex_destroy(&arg.lock);
        check(0, "start slow-header sender");
        return;
    }
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    response_t response = {0};
    int result = read_response(fd, &response);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    pthread_mutex_lock(&arg.lock);
    arg.stop = 1;
    pthread_mutex_unlock(&arg.lock);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    pthread_join(sender, NULL);
    pthread_mutex_destroy(&arg.lock);
    double elapsed = (finished.tv_sec - started.tv_sec) * 1000.0 +
                     (finished.tv_nsec - started.tv_nsec) / 1000000.0;
    check(result == 0 && response.status == 400 && elapsed >= 4000.0 &&
          elapsed < 7000.0,
          "slow-trickle request headers hit absolute deadline and release worker");
    free_response(&response);
}

static void test_origin_response_deadline(void)
{
    struct timespec started, finished;
    response_t response = {0};
    clock_gettime(CLOCK_MONOTONIC, &started);
    int result = request_proxy("127.0.0.1", origin_port, "/trickle", 0, &response);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    double elapsed = (finished.tv_sec - started.tv_sec) * 1000.0 +
                     (finished.tv_nsec - started.tv_nsec) / 1000000.0;
    size_t body_length = response.length >= response.header_length
                             ? response.length - response.header_length : 0;
    int deadline_ok = result == 0 && response.status == 200 && body_length < 12 &&
                      elapsed >= 7000.0 && elapsed < 11000.0;
    if (!deadline_ok)
        fprintf(stderr, "origin deadline diagnostic result=%d status=%d body=%zu elapsed=%.1fms\n",
                result, response.status, body_length, elapsed);
    check(deadline_ok,
          "slow-trickle origin response is bounded by total deadline");
    free_response(&response);
    result = request_proxy("127.0.0.1", origin_port, "/hello", 0, &response);
    check(result == 0 && response.status == 200,
          "proxy remains available after total origin-response deadline");
    free_response(&response);
}

static void check_metrics_file(const char *path)
{
    FILE *file = fopen(path, "r");
    char header[256], line[256];
    long total, success, failed, hits, misses, bytes;
    double latency;
    int valid = file != NULL && fgets(header, sizeof(header), file) != NULL &&
                fgets(line, sizeof(line), file) != NULL &&
                sscanf(line, "%ld,%ld,%ld,%ld,%ld,%ld,%lf",
                       &total, &success, &failed, &hits, &misses,
                       &bytes, &latency) == 7;
    if (file != NULL) fclose(file);
    check(valid && success + failed == total && hits + misses == total &&
          total > 0 && bytes > 0 && latency >= 0.0,
          "proxy metrics match real workload invariants");
}

static void check_log(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) { check(0, "proxy log exists"); return; }
    char line[4096];
    int hit = 0, forbidden = 0, gateway = 0;
    int bad_request = 0, timeout = 0, success = 0, miss = 0, overload = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        hit |= strstr(line, "Cache=HIT") != NULL;
        miss |= strstr(line, "Cache=MISS") != NULL;
        forbidden |= strstr(line, "Status=403") != NULL;
        gateway |= strstr(line, "Status=502") != NULL;
        bad_request |= strstr(line, "Status=400") != NULL;
        timeout |= strstr(line, "Status=504") != NULL;
        overload |= strstr(line, "Status=503") != NULL;
        success |= strstr(line, "Status=200") != NULL;
    }
    fclose(file);
        check(hit && miss && forbidden && gateway && bad_request && timeout &&
            overload && success,
            "logger records real HIT, MISS, BLOCK, malformed, gateway, timeout, overload, and success traffic");
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s proxy-binary origin-fixture access-config benchmark-binary\n", argv[0]);
        return 2;
    }
    char cwd[4096], log_path[8192];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return 2;
    snprintf(log_path, sizeof(log_path), "%s/proxy.log", cwd);
    pid_t origin = -1;
    int origin_result = start_origin(argv[2], &origin_port, &origin);
    check(origin_result == 0, "local origin starts");
    if (origin <= 0) return 1;
    pid_t second_origin = -1;
    int second_origin_result = start_origin(argv[2], &second_origin_port, &second_origin);
    check(second_origin_result == 0, "second local origin starts on a distinct port");
    if (second_origin <= 0) {
        kill(origin, SIGTERM); waitpid(origin, NULL, 0); return 1;
    }
    proxy_port = unused_port();
    pid_t proxy = start_proxy(argv[1], argv[3], proxy_port, 32);
    int proxy_ready = proxy > 0 && wait_proxy(proxy, proxy_port) == 0;
    check(proxy_ready, "proxy starts locally");
    if (!proxy_ready) {
        kill(origin, SIGTERM); waitpid(origin, NULL, 0);
        kill(second_origin, SIGTERM); waitpid(second_origin, NULL, 0);
        return 1;
    }

    response_t response = {0};
    int result = request_proxy("127.0.0.1", origin_port, "/hello", 1, &response);
    int hello_ok = result == 0 && response.status == 200 &&
                   response_body_equals(&response, "hello from local origin\n");
    if (!hello_ok)
        fprintf(stderr, "hello diagnostic result=%d status=%d wire=%zu header=%zu body=%zu\n",
                result, response.status, response.length, response.header_length,
                response.length >= response.header_length ?
                    response.length - response.header_length : 0);
    check(hello_ok,
          "fragmented HTTP request forwards correctly");
    free_response(&response);

    unsigned long count = 0;
    response_t first = {0}, second = {0};
    request_proxy("127.0.0.1", origin_port, "/cache?x=1", 0, &first);
    request_proxy("127.0.0.1", origin_port, "/cache?x=1", 0, &second);
    int same_response = first.status == 200 && second.status == 200 &&
                        first.length == second.length &&
                        memcmp(first.data, second.data,
                               first.length < second.length ? first.length : second.length) == 0;
    if (!same_response)
        fprintf(stderr, "cache response diagnostic first=%d/%zu second=%d/%zu headers=%zu/%zu\n",
                first.status, first.length, second.status, second.length,
                first.header_length, second.header_length);
    check(same_response,
          "cache returns identical complete response");
    check(fetch_count("/cache?x=1", &count) == 0 && count == 1,
          "cache HIT proven by unchanged origin counter");
    free_response(&first); free_response(&second);

    response_t other_host = {0};
    request_proxy("localhost", origin_port, "/cache?x=1", 0, &other_host);
    check(fetch_count("/cache?x=1", &count) == 0 && count == 2,
          "same path under another authority is a distinct cache MISS");
    free_response(&other_host);
        response_t other_port = {0};
        request_proxy("127.0.0.1", second_origin_port, "/cache?x=1", 0, &other_port);
        unsigned long first_port_count = 0, second_port_count = 0;
        check(fetch_count_at(origin_port, "/cache?x=1", &first_port_count) == 0 &&
            fetch_count_at(second_origin_port, "/cache?x=1", &second_port_count) == 0 &&
            first_port_count == 2 && second_port_count == 1,
            "same host/path on different ports uses separate cache entries");
        free_response(&other_port);
    response_t query1 = {0}, query2 = {0};
    request_proxy("127.0.0.1", origin_port, "/query?q=1", 0, &query1);
    request_proxy("127.0.0.1", origin_port, "/query?q=2", 0, &query2);
    check(!response_body_equals(&query1, "origin:/query?q=2\n") &&
          !response_body_equals(&query2, "origin:/query?q=1\n"),
          "query variants remain distinct");
    free_response(&query1); free_response(&query2);

        response_t path_a = {0}, path_b = {0};
        request_proxy("127.0.0.1", origin_port, "/path-a", 0, &path_a);
        request_proxy("127.0.0.1", origin_port, "/path-b", 0, &path_b);
        unsigned long path_a_count = 0, path_b_count = 0;
        check(fetch_count("/path-a", &path_a_count) == 0 && path_a_count == 1 &&
            fetch_count("/path-b", &path_b_count) == 0 && path_b_count == 1,
            "different paths are separate cache misses");
        free_response(&path_a); free_response(&path_b);

        response_t not_found_a = {0}, not_found_b = {0};
        request_proxy("127.0.0.1", origin_port, "/status", 0, &not_found_a);
        request_proxy("127.0.0.1", origin_port, "/status", 0, &not_found_b);
        unsigned long status_count = 0;
        check(not_found_a.status == 404 && not_found_b.status == 404 &&
            fetch_count("/status", &status_count) == 0 && status_count == 2,
            "non-success response is not cached");
        free_response(&not_found_a); free_response(&not_found_b);

        response_t oversized_a = {0}, oversized_b = {0};
        request_proxy("127.0.0.1", origin_port, "/too-large", 0, &oversized_a);
        request_proxy("127.0.0.1", origin_port, "/too-large", 0, &oversized_b);
        unsigned long oversized_count = 0;
        check(oversized_a.status == 200 && oversized_b.status == 200 &&
            fetch_count("/too-large", &oversized_count) == 0 && oversized_count == 2,
            "response over 1 MiB is forwarded but not cached");
        free_response(&oversized_a); free_response(&oversized_b);

    response_t expiry = {0};
    request_proxy("127.0.0.1", origin_port, "/expire", 0, &expiry);
    free_response(&expiry);
    struct timespec pause = { .tv_sec = 3, .tv_nsec = 0 };
    nanosleep(&pause, NULL);
    request_proxy("127.0.0.1", origin_port, "/expire", 0, &expiry);
    check(fetch_count("/expire", &count) == 0 && count == 2,
          "expired cache entry is refetched");
    free_response(&expiry);

    unsigned long before = 0, after = 0;
    fetch_count("/never", &before);
    request_proxy("blocked.test", origin_port, "/never", 0, &response);
    check(response.status == 403, "blocked hostname receives HTTP 403");
    free_response(&response);
    fetch_count("/never", &after);
    check(before == after, "blocked request proven not to reach origin");
    request_proxy("blocked.test.", origin_port, "/never", 0, &response);
    check(response.status == 403, "terminal-dot blocked hostname also receives HTTP 403");
    free_response(&response);
    fetch_count("/never", &after);
    check(before == after, "terminal-dot blocked request also avoids origin");
        result = request_proxy("127.0.0.1", origin_port, "/hello", 0, &response);
        check(result == 0 && response.status == 200,
            "proxy continues serving after access-control BLOCK");
        free_response(&response);

    check(request_raw("GET / HTTP/1.1\r\n\r\n", &response) == 0 && response.status == 400,
          "malformed request receives HTTP 400");
    free_response(&response);
        char origin_form[256];
        snprintf(origin_form, sizeof(origin_form),
             "GET /hello HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
             origin_port);
        check(request_raw(origin_form, &response) == 0 && response.status == 200 &&
            response_body_equals(&response, "hello from local origin\n"),
            "origin-form HTTP/1.0 request forwards correctly");
        free_response(&response);
    check(request_raw("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                      &response) == 0 && response.status == 400,
          "unsupported method receives HTTP 400");
    free_response(&response);
    check(request_raw("GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n\r\nx",
                      &response) == 0 && response.status == 400,
          "request body is rejected cleanly");
    free_response(&response);
    char *oversized = malloc(10000);
    if (oversized != NULL) {
        int prefix = snprintf(oversized, 10000,
                              "GET / HTTP/1.1\r\nHost: localhost\r\nX-Fill: ");
        memset(oversized + prefix, 'a', 9999 - (size_t)prefix);
        oversized[9999] = '\0';
        int oversized_result = request_raw(oversized, &response);
        if (oversized_result != 0 || response.status != 400)
            fprintf(stderr, "oversized diagnostic result=%d status=%d wire=%zu header=%zu\n",
                    oversized_result, response.status, response.length,
                    response.header_length);
        check(oversized_result == 0 && response.status == 400,
              "oversized request headers are rejected");
        free(oversized);
    } else {
        check(0, "allocate oversized request test");
    }
    free_response(&response);
    int closed_port = unused_port();
    request_proxy("127.0.0.1", closed_port, "/bad", 0, &response);
    check(response.status == 502, "unreachable origin receives HTTP 502");
    free_response(&response);
        result = request_proxy("127.0.0.1", origin_port, "/hello", 0, &response);
        check(result == 0 && response.status == 200,
            "proxy continues serving after unreachable origin");
        free_response(&response);
    request_proxy("127.0.0.1", origin_port, "/delay", 0, &response);
    check(response.status == 504, "delayed origin response receives HTTP 504");
    free_response(&response);
    result = request_proxy("127.0.0.1", origin_port, "/hello", 0, &response);
    check(result == 0 && response.status == 200,
          "proxy continues serving after origin response timeout");
    free_response(&response);

    request_proxy("127.0.0.1", origin_port, "/chunked", 0, &response);
        int chunked_ok = response.status == 200 &&
                   response_contains(&response, "Transfer-Encoding: chunked") &&
                   response_body_equals(&response,
                     "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    free_response(&response);
        request_proxy("127.0.0.1", origin_port, "/chunked", 0, &response);
        unsigned long chunked_count = 0;
        check(chunked_ok && response.status == 200 &&
            fetch_count("/chunked", &chunked_count) == 0 && chunked_count == 2,
            "chunked response is intact and repeated requests refetch (not cached)");
        free_response(&response);
        request_proxy("127.0.0.1", origin_port, "/chunked-with-length", 0, &response);
        check(response.status == 200 && response_contains(&response, "Transfer-Encoding: chunked") &&
            response_body_equals(&response, "4\r\nWiki\r\n0\r\n\r\n"),
            "chunked response with conflicting Content-Length is preserved intact");
        free_response(&response);

    result = request_proxy("127.0.0.1", origin_port, "/large", 0, &response);
    int large_ok = result == 0 && response.status == 200 &&
                   response.length - response.header_length == LARGE_SIZE;
    size_t large_length = response.length >= response.header_length ?
                          response.length - response.header_length : 0;
    if (large_ok) {
        for (size_t i = 0; i < LARGE_SIZE; ++i)
            if (response.data[response.header_length + i] != (unsigned char)('a' + i % 26)) {
                large_ok = 0; break;
            }
    }
    if (!large_ok)
        fprintf(stderr, "large diagnostic result=%d status=%d wire=%zu header=%zu body=%zu\n",
                result, response.status, response.length, response.header_length,
                large_length);
    check(large_ok, "1 MiB response forwarded byte-for-byte");
    free_response(&response);

    run_concurrency_levels();
    test_worker_capacity();
    test_client_header_deadline();
    test_origin_response_deadline();
    test_client_disconnect();
    kill(proxy, SIGTERM); waitpid(proxy, NULL, 0);
    check_metrics_file("metrics.csv");
    proxy_port = unused_port();
    proxy = start_proxy(argv[1], argv[3], proxy_port, 0);
    proxy_ready = proxy > 0 && wait_proxy(proxy, proxy_port) == 0;
    check(proxy_ready, "benchmark proxy starts with cache disabled");
    if (proxy_ready)
        run_real_benchmarks(argv[4]);
    if (proxy > 0) { kill(proxy, SIGTERM); waitpid(proxy, NULL, 0); }
    kill(origin, SIGTERM); waitpid(origin, NULL, 0);
    kill(second_origin, SIGTERM); waitpid(second_origin, NULL, 0);
    check_log(log_path);
    printf("Integration failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
