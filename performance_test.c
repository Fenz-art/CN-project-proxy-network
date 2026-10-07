#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_BUFFER_SIZE 8192
#define REQUEST_BUFFER_SIZE 4096
#define STATUS_LINE_SIZE 1024
#define RESPONSE_HEADER_SIZE 16384
#define MAX_CLIENTS 100000

typedef struct {
    const char *mode;
    const char *connect_host;
    int connect_port;
    const char *origin_host;
    int origin_port;
    const char *path;
} BenchmarkConfig;

typedef struct {
    const BenchmarkConfig *config;
    int completed;
    int success;
    int status_code;
    size_t response_bytes;
    double latency_ms;
    int thread_started;
} ClientResult;

static int parse_integer(const char *text, int minimum, int maximum, int *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int has_header_injection(const char *text)
{
    return strchr(text, '\r') != NULL || strchr(text, '\n') != NULL;
}

static int connect_endpoint(const char *host, int port)
{
    char service[6];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int lookup = getaddrinfo(host, service, &hints, &addresses);
    if (lookup != 0)
        return -1;

    int connected_fd = -1;
    for (struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);
        if (fd < 0)
            continue;
        struct timeval timeout = { .tv_sec = 15, .tv_usec = 0 };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) != 0) {
            close(fd);
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected_fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected_fd;
}

static int send_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
#ifdef MSG_NOSIGNAL
        ssize_t sent = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
#else
        ssize_t sent = send(fd, data + offset, length - offset, 0);
#endif
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (sent == 0)
            return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static int parse_status_line(const char *line, int *status)
{
    if (strlen(line) < 12 ||
        (strncmp(line, "HTTP/1.0 ", 9) != 0 &&
         strncmp(line, "HTTP/1.1 ", 9) != 0) ||
        line[9] < '0' || line[9] > '9' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (line[12] != '\0' && line[12] != '\r' && line[12] != ' '))
        return -1;
    *status = (line[9] - '0') * 100 + (line[10] - '0') * 10 +
              line[11] - '0';
    return 0;
}

static int parse_response_headers(char *headers, size_t length, int *status,
                                  size_t *content_length, int *has_content_length)
{
    if (length < 4 || memcmp(headers + length - 4, "\r\n\r\n", 4) != 0)
        return -1;
    headers[length] = '\0';
    char *line_end = strstr(headers, "\r\n");
    if (line_end == NULL)
        return -1;
    *line_end = '\0';
    if (parse_status_line(headers, status) != 0)
        return -1;
    char *cursor = line_end + 2;
    char *headers_end = headers + length;
    while (cursor < headers_end) {
        line_end = strstr(cursor, "\r\n");
        if (line_end == NULL)
            return -1;
        if (line_end == cursor)
            return line_end + 2 == headers + length ? 0 : -1;
        *line_end = '\0';
        char *colon = strchr(cursor, ':');
        if (colon == NULL || colon == cursor || *cursor == ' ' || *cursor == '\t')
            return -1;
        if ((size_t)(colon - cursor) == 14 &&
            strncasecmp(cursor, "Content-Length", 14) == 0) {
            if (*has_content_length)
                return -1;
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                ++value;
            if (*value == '\0')
                return -1;
            size_t parsed = 0;
            for (; *value != '\0'; ++value) {
                if (!isdigit((unsigned char)*value) ||
                    parsed > (SIZE_MAX - (size_t)(*value - '0')) / 10)
                    return -1;
                parsed = parsed * 10 + (size_t)(*value - '0');
            }
            *content_length = parsed;
            *has_content_length = 1;
        }
        cursor = line_end + 2;
    }
    return -1;
}

static void *run_client(void *opaque)
{
    ClientResult *result = opaque;
    const BenchmarkConfig *config = result->config;
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int fd = connect_endpoint(config->connect_host, config->connect_port);
    if (fd < 0)
        goto done;

    char authority[512];
    int authority_length;
    if (strchr(config->origin_host, ':') != NULL)
        authority_length = snprintf(authority, sizeof(authority), "[%s]:%d",
                                    config->origin_host, config->origin_port);
    else
        authority_length = snprintf(authority, sizeof(authority), "%s:%d",
                                    config->origin_host, config->origin_port);
    if (authority_length < 0 || (size_t)authority_length >= sizeof(authority)) {
        close(fd);
        goto done;
    }

    char request_target[REQUEST_BUFFER_SIZE];
    int target_length;
    if (strcmp(config->mode, "proxy") == 0) {
        if (strchr(config->origin_host, ':') != NULL)
            target_length = snprintf(request_target, sizeof(request_target),
                                     "http://[%s]:%d%s", config->origin_host,
                                     config->origin_port, config->path);
        else
            target_length = snprintf(request_target, sizeof(request_target),
                                     "http://%s:%d%s", config->origin_host,
                                     config->origin_port, config->path);
    } else {
        target_length = snprintf(request_target, sizeof(request_target), "%s",
                                 config->path);
    }
    if (target_length < 0 || (size_t)target_length >= sizeof(request_target)) {
        close(fd);
        goto done;
    }

    char request[REQUEST_BUFFER_SIZE];
    int request_length = snprintf(request, sizeof(request),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\n"
                                  "Connection: close\r\n\r\n",
                                  request_target, authority);
    if (request_length < 0 || (size_t)request_length >= sizeof(request) ||
        send_all(fd, request, (size_t)request_length) != 0) {
        close(fd);
        goto done;
    }

    char response_headers[RESPONSE_HEADER_SIZE + 1];
    size_t header_prefix_length = 0;
    size_t header_length = 0;
    size_t expected_body_length = 0;
    int has_content_length = 0;
    int headers_complete = 0;
    char response[RESPONSE_BUFFER_SIZE];
    for (;;) {
        ssize_t received = recv(fd, response, sizeof(response), 0);
        if (received == 0)
            break;
        if (received < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            goto done;
        }
        result->response_bytes += (size_t)received;
        if (!headers_complete) {
            size_t available = RESPONSE_HEADER_SIZE - header_prefix_length;
            size_t copy_length = (size_t)received < available
                                     ? (size_t)received : available;
            memcpy(response_headers + header_prefix_length, response, copy_length);
            header_prefix_length += copy_length;
            response_headers[header_prefix_length] = '\0';
            char *header_end = strstr(response_headers, "\r\n\r\n");
            if (header_end != NULL) {
                header_length = (size_t)(header_end - response_headers) + 4;
                if (parse_response_headers(response_headers, header_length,
                                           &result->status_code,
                                           &expected_body_length,
                                           &has_content_length) != 0) {
                    close(fd);
                    goto done;
                }
                headers_complete = 1;
            } else if (header_prefix_length == RESPONSE_HEADER_SIZE) {
                close(fd);
                goto done;
            }
        }
    }
    close(fd);
    result->success = headers_complete && result->response_bytes >= header_length &&
                      (!has_content_length ||
                       result->response_bytes - header_length >= expected_body_length) &&
                      result->status_code >= 200 && result->status_code < 300;

done:
    clock_gettime(CLOCK_MONOTONIC, &finish);
    result->latency_ms = (finish.tv_sec - start.tv_sec) * 1000.0 +
                         (finish.tv_nsec - start.tv_nsec) / 1000000.0;
    result->completed = 1;
    return NULL;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s direct <origin_host> <origin_port> <path> <clients>\n"
            "  %s proxy <proxy_host> <proxy_port> <origin_host> <origin_port> <path> <clients>\n",
            program, program);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    BenchmarkConfig config;
    const char *client_count_text;
    if (argc == 6 && strcmp(argv[1], "direct") == 0) {
        config.mode = "direct";
        config.connect_host = argv[2];
        config.origin_host = argv[2];
        if (parse_integer(argv[3], 1, 65535, &config.connect_port) != 0) {
            print_usage(argv[0]);
            return 2;
        }
        config.origin_port = config.connect_port;
        config.path = argv[4];
        client_count_text = argv[5];
    } else if (argc == 8 && strcmp(argv[1], "proxy") == 0) {
        config.mode = "proxy";
        config.connect_host = argv[2];
        config.origin_host = argv[4];
        if (parse_integer(argv[3], 1, 65535, &config.connect_port) != 0 ||
            parse_integer(argv[5], 1, 65535, &config.origin_port) != 0) {
            print_usage(argv[0]);
            return 2;
        }
        config.path = argv[6];
        client_count_text = argv[7];
    } else {
        print_usage(argv[0]);
        return 2;
    }

    int clients;
    if (parse_integer(client_count_text, 1, MAX_CLIENTS, &clients) != 0 ||
        config.connect_host[0] == '\0' || config.origin_host[0] == '\0' ||
        config.path[0] != '/' || has_header_injection(config.connect_host) ||
        has_header_injection(config.origin_host) || has_header_injection(config.path)) {
        fprintf(stderr, "Invalid host, path, port, or client count.\n");
        return 2;
    }

    pthread_t *threads = calloc((size_t)clients, sizeof(*threads));
    ClientResult *results = calloc((size_t)clients, sizeof(*results));
    if (threads == NULL || results == NULL) {
        perror("Unable to allocate benchmark client state");
        free(threads);
        free(results);
        return 1;
    }

    struct timespec total_start, total_finish;
    clock_gettime(CLOCK_MONOTONIC, &total_start);
    int attempted = clients;
    int created = 0;
    for (int i = 0; i < clients; ++i) {
        results[i].config = &config;
        int error = pthread_create(&threads[i], NULL, run_client, &results[i]);
        if (error != 0) {
            fprintf(stderr, "Unable to create client thread %d: %s\n", i,
                    strerror(error));
        } else {
            results[i].thread_started = 1;
            ++created;
        }
    }
    int successful = 0;
    int completed = 0;
    for (int i = 0; i < clients; ++i) {
        if (results[i].thread_started) {
            int error = pthread_join(threads[i], NULL);
            if (error != 0) {
                fprintf(stderr, "Unable to join client thread %d: %s\n", i,
                        strerror(error));
                continue;
            }
            ++completed;
            if (results[i].success)
                ++successful;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &total_finish);

    int failed = attempted - successful;
    double total_ms = (total_finish.tv_sec - total_start.tv_sec) * 1000.0 +
                      (total_finish.tv_nsec - total_start.tv_nsec) / 1000000.0;
    double latency_total = 0.0;
    size_t response_bytes_total = 0;
    for (int i = 0; i < clients; ++i) {
        if (results[i].completed)
            latency_total += results[i].latency_ms;
        if (results[i].success)
            response_bytes_total += results[i].response_bytes;
    }
    double average_latency = completed > 0 ? latency_total / completed : 0.0;
    double throughput = total_ms > 0.0 ? successful * 1000.0 / total_ms : 0.0;

    printf("Mode              : %s\n", config.mode);
    printf("Attempted requests: %d\n", attempted);
    printf("Successful       : %d\n", successful);
    printf("Failed           : %d\n", failed);
    printf("Successful response bytes: %zu\n", response_bytes_total);
    printf("Total elapsed    : %.2f ms\n", total_ms);
    printf("Average latency   : %.2f ms\n", average_latency);
    printf("Throughput        : %.2f successful requests/sec\n", throughput);
    printf("Created threads   : %d\n", created);

    free(results);
    free(threads);
    return failed == 0 ? 0 : 1;
}