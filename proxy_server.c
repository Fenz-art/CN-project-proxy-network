#define _POSIX_C_SOURCE 200809L
#include "proxy_server.h"

#include "http_tcp_handler.h"
#include "logger.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <semaphore.h>

#define MAX_RESPONSE_HEADER 16384
#define ORIGIN_READ_SIZE 16384
#define DEFAULT_WORKERS 64
#define CLIENT_READ_TIMEOUT_SECONDS 5
#define ORIGIN_RESPONSE_TIMEOUT_SECONDS 2
#define CLIENT_HEADER_DEADLINE_MS 5000
#define ORIGIN_RESPONSE_DEADLINE_MS 8000
#define CLIENT_WRITE_DEADLINE_MS 5000
#define ORIGIN_WRITE_DEADLINE_MS 5000

typedef struct {
    struct timespec at;
} io_deadline_t;

typedef struct {
    const proxy_server_config_t *config;
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
} worker_arg_t;

typedef struct {
    int status;
    size_t content_length;
    int has_content_length;
    int chunked;
} response_info_t;

static volatile sig_atomic_t stopping;
static sem_t worker_slots;
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t active_condition = PTHREAD_COND_INITIALIZER;
static size_t active_workers;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int deadline_after_ms(io_deadline_t *deadline, int timeout_ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, &deadline->at) != 0)
        return -1;
    deadline->at.tv_sec += timeout_ms / 1000;
    deadline->at.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->at.tv_nsec >= 1000000000L) {
        ++deadline->at.tv_sec;
        deadline->at.tv_nsec -= 1000000000L;
    }
    return 0;
}

static int wait_for_io(int fd, short events, const io_deadline_t *deadline)
{
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -1;
        int64_t remaining_ns =
            (int64_t)(deadline->at.tv_sec - now.tv_sec) * 1000000000LL +
            deadline->at.tv_nsec - now.tv_nsec;
        if (remaining_ns <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int timeout_ms = (int)((remaining_ns + 999999LL) / 1000000LL);
        struct pollfd descriptor = { .fd = fd, .events = events };
        int ready = poll(&descriptor, 1, timeout_ms);
        if (ready > 0) {
            if ((descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0)
                return 0;
            continue;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
    }
}

static int send_all_until(int fd, const void *data, size_t length,
                          const io_deadline_t *deadline)
{
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t sent = send(fd, bytes + offset, length - offset, flags);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (wait_for_io(fd, POLLOUT, deadline) == 0)
                    continue;
            }
            return -1;
        }
        if (sent == 0) {
            errno = EPIPE;
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static int send_all_timeout(int fd, const void *data, size_t length, int timeout_ms)
{
    io_deadline_t deadline;
    if (deadline_after_ms(&deadline, timeout_ms) != 0)
        return -1;
    return send_all_until(fd, data, length, &deadline);
}

static ssize_t recv_until(int fd, void *buffer, size_t length,
                          const io_deadline_t *deadline)
{
    for (;;) {
        if (wait_for_io(fd, POLLIN, deadline) != 0)
            return -1;
        ssize_t received = recv(fd, buffer, length, 0);
        if (received < 0 && errno == EINTR)
            continue;
        return received;
    }
}

static int set_socket_timeouts(int fd, int receive_seconds, int send_seconds)
{
    struct timeval receive_timeout = {
        .tv_sec = receive_seconds,
        .tv_usec = 0
    };
    struct timeval send_timeout = {
        .tv_sec = send_seconds,
        .tv_usec = 0
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout)) != 0)
        return -1;
    return 0;
}

static int is_timeout_error(int error)
{
    return error == ETIMEDOUT || error == EAGAIN || error == EWOULDBLOCK;
}

static int send_http_error(int client_fd, int status, const char *reason,
                           const char *body)
{
    char response[1024];
    size_t body_length = strlen(body);
    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          status, reason, body_length, body);
    if (length < 0 || (size_t)length >= sizeof(response))
        return -1;
    return send_all_timeout(client_fd, response, (size_t)length,
                            CLIENT_WRITE_DEADLINE_MS) == 0 ? length : -1;
}

static void record_proxy_request(const proxy_server_config_t *config,
                                 const char *client_ip, const char *method,
                                 const char *host, const char *path, int status,
                                 int success, int cache_hit, size_t bytes,
                                 const struct timespec *started)
{
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &finished);
    double latency_ms = (finished.tv_sec - started->tv_sec) * 1000.0 +
                        (finished.tv_nsec - started->tv_nsec) / 1000000.0;
    log_request(client_ip, method, host, path, status, cache_hit,
                (long)bytes, latency_ms);
    record_request(config->metrics, success, cache_hit, (long)bytes, latency_ms);
}

static void record_rejected_client(const proxy_server_config_t *config,
                                   const struct sockaddr_storage *peer,
                                   socklen_t peer_length, int status,
                                   int response_bytes,
                                   const struct timespec *started)
{
    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    getnameinfo((const struct sockaddr *)peer, peer_length, client_ip,
                sizeof(client_ip), NULL, 0, NI_NUMERICHOST);
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &finished);
    double latency_ms = (finished.tv_sec - started->tv_sec) * 1000.0 +
                        (finished.tv_nsec - started->tv_nsec) / 1000000.0;
    long bytes = response_bytes > 0 ? response_bytes : 0;
    log_request(client_ip, "-", "-", "-", status, 0, bytes, latency_ms);
    record_request(config->metrics, 0, 0, bytes, latency_ms);
}

static int contains_case_insensitive(const char *text, size_t length,
                                     const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length > length)
        return 0;
    for (size_t i = 0; i + needle_length <= length; ++i) {
        if (strncasecmp(text + i, needle, needle_length) == 0)
            return 1;
    }
    return 0;
}

static int read_request_headers(int client_fd, size_t limit,
                                char *buffer, size_t *header_length,
                                int *extra_data,
                                const io_deadline_t *deadline)
{
    size_t used = 0;
    *extra_data = 0;
    while (used < limit) {
        ssize_t received = recv_until(client_fd, buffer + used, limit - used,
                          deadline);
        if (received == 0)
            return -1;
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        used += (size_t)received;
        buffer[used] = '\0';
        char *end = strstr(buffer, "\r\n\r\n");
        if (end != NULL) {
            *header_length = (size_t)(end - buffer) + 4;
            *extra_data = used > *header_length;
            buffer[*header_length] = '\0';
            return 0;
        }
    }
    errno = EMSGSIZE;
    return -1;
}

static int request_has_unsupported_body(const char *request)
{
    const char *line = strstr(request, "\r\n");
    if (line == NULL)
        return 1;
    line += 2;
    while (*line != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL)
            return 1;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon != NULL) {
            size_t name_length = (size_t)(colon - line);
            if (name_length == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                const char *value = colon + 1;
                while (value < end && (*value == ' ' || *value == '\t'))
                    ++value;
                if (value < end && !(end - value == 1 && *value == '0'))
                    return 1;
            }
            if (name_length == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
                return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int make_cache_key(const http_request_t *request, char *key, size_t capacity)
{
    char normalized_host[sizeof(request->host)];
    size_t host_length = strlen(request->host);
    if (host_length >= sizeof(normalized_host))
        return -1;
    for (size_t i = 0; i <= host_length; ++i)
        normalized_host[i] = (char)tolower((unsigned char)request->host[i]);
    int length = snprintf(key, capacity, "%s:%d%s", normalized_host,
                          request->port, request->path);
    return length < 0 || (size_t)length >= capacity ? -1 : 0;
}

static int parse_response_headers(const unsigned char *header, size_t length,
                                  response_info_t *info)
{
    if (length < 4 || memcmp(header + length - 4, "\r\n\r\n", 4) != 0)
        return -1;
    char *copy = malloc(length + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, header, length);
    copy[length] = '\0';
    char *line_end = strstr(copy, "\r\n");
    int parsed_status = 0;
    if (line_end == NULL || sscanf(copy, "HTTP/%*s %d", &parsed_status) != 1 ||
        parsed_status < 100 || parsed_status > 599) {
        free(copy);
        return -1;
    }
    info->status = parsed_status;
    info->has_content_length = 0;
    info->chunked = 0;
    info->content_length = 0;
    char *cursor = line_end + 2;
    while (*cursor != '\0') {
        line_end = strstr(cursor, "\r\n");
        if (line_end == NULL || line_end == cursor)
            break;
        char *colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon == NULL) {
            free(copy);
            return -1;
        }
        size_t name_len = (size_t)(colon - cursor);
        if (name_len == 14 && strncasecmp(cursor, "Content-Length", 14) == 0) {
            if (info->has_content_length) {
                free(copy);
                return -1;
            }
            char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t'))
                ++value;
            size_t parsed = 0;
            if (value == line_end) {
                free(copy);
                return -1;
            }
            for (; value < line_end; ++value) {
                if (!isdigit((unsigned char)*value) ||
                    parsed > (SIZE_MAX - (size_t)(*value - '0')) / 10) {
                    free(copy);
                    return -1;
                }
                parsed = parsed * 10 + (size_t)(*value - '0');
            }
            info->content_length = parsed;
            info->has_content_length = 1;
        } else if (name_len == 17 && strncasecmp(cursor, "Transfer-Encoding", 17) == 0) {
            if (contains_case_insensitive(colon + 1,
                                          (size_t)(line_end - colon - 1),
                                          "chunked"))
                info->chunked = 1;
        }
        cursor = line_end + 2;
    }
    if (info->chunked) {
        info->has_content_length = 0;
        info->content_length = 0;
    }
    free(copy);
    return 0;
}

static int append_capture(unsigned char **capture, size_t *used, size_t *capacity,
                          const unsigned char *data, size_t length, size_t maximum)
{
    if (*used > maximum || length > maximum - *used)
        return 1;
    size_t required = *used + length;
    if (required > *capacity) {
        size_t next = *capacity == 0 ? 4096 : *capacity;
        while (next < required) {
            if (next > maximum / 2) {
                next = maximum;
                break;
            }
            next *= 2;
        }
        unsigned char *grown = realloc(*capture, next);
        if (grown == NULL)
            return -1;
        *capture = grown;
        *capacity = next;
    }
    memcpy(*capture + *used, data, length);
    *used += length;
    return 0;
}

static int forward_origin_response(int origin_fd, int client_fd,
                                   const proxy_server_config_t *config,
                                   int *status, size_t *response_bytes,
                                   unsigned char **captured, size_t *captured_len)
{
    unsigned char header[MAX_RESPONSE_HEADER];
    size_t header_used = 0;
    size_t header_length = 0;
    int headers_ready = 0;
    int cache_eligible = 0;
    int capture_disabled = 0;
    size_t body_seen = 0;
    size_t capture_capacity = 0;
    response_info_t info = {0};
    unsigned char chunk[ORIGIN_READ_SIZE];
    size_t initial_body = 0;
    io_deadline_t response_deadline;
    io_deadline_t client_write_deadline;

    if (deadline_after_ms(&response_deadline,
                          ORIGIN_RESPONSE_DEADLINE_MS) != 0 ||
        deadline_after_ms(&client_write_deadline,
                          CLIENT_WRITE_DEADLINE_MS) != 0)
        return -1;

    *captured = NULL;
    *captured_len = 0;
    *response_bytes = 0;
    for (;;) {
        size_t remaining = headers_ready && info.has_content_length
                               ? info.content_length - body_seen
                               : sizeof(chunk);
        if (headers_ready && info.has_content_length && remaining == 0)
            break;
        size_t read_size = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (!headers_ready) {
            if (header_used == sizeof(header)) {
                errno = EPROTO;
                goto failure;
            }
            read_size = sizeof(header) - header_used;
            ssize_t received = recv_until(origin_fd, header + header_used,
                                          read_size, &response_deadline);
            if (received < 0) {
                if (errno == EINTR)
                    continue;
                goto failure;
            }
            if (received == 0) {
                errno = ECONNRESET;
                goto failure;
            }
            header_used += (size_t)received;
            unsigned char *end = NULL;
            if (header_used >= 4) {
                for (size_t i = 3; i < header_used; ++i) {
                    if (memcmp(header + i - 3, "\r\n\r\n", 4) == 0) {
                        end = header + i + 1;
                        break;
                    }
                }
            }
            if (end == NULL)
                continue;
            header_length = (size_t)(end - header);
            if (parse_response_headers(header, header_length, &info) != 0) {
                errno = EPROTO;
                goto failure;
            }
            headers_ready = 1;
            *status = info.status;
            cache_eligible = info.has_content_length && !info.chunked &&
                             info.status >= 200 && info.status < 300 &&
                             header_length <= config->max_cached_response &&
                             info.content_length <= config->max_cached_response - header_length;
            initial_body = header_used - header_length;
            if (info.has_content_length && initial_body > info.content_length) {
                errno = EPROTO;
                goto failure;
            }
            if (send_all_until(client_fd, header, header_length,
                               &client_write_deadline) != 0)
                goto failure;
            *response_bytes += header_length;
            if (cache_eligible) {
                int appended = append_capture(captured, captured_len,
                                              &capture_capacity, header, header_length,
                                              config->max_cached_response);
                if (appended != 0) {
                    free(*captured);
                    *captured = NULL;
                    *captured_len = 0;
                    capture_disabled = 1;
                }
            }
            body_seen += initial_body;
            if (initial_body > 0) {
                if (send_all_until(client_fd, header + header_length, initial_body,
                                   &client_write_deadline) != 0)
                    goto failure;
                if (cache_eligible && !capture_disabled) {
                    int appended = append_capture(captured, captured_len,
                                                  &capture_capacity,
                                                  header + header_length,
                                                  initial_body,
                                                  config->max_cached_response);
                    if (appended != 0) {
                        free(*captured);
                        *captured = NULL;
                        *captured_len = 0;
                        capture_disabled = 1;
                    }
                }
                *response_bytes += initial_body;
            }
            continue;
        }

        ssize_t received = recv_until(origin_fd, chunk, read_size,
                          &response_deadline);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            goto failure;
        }
        if (received == 0) {
            if (info.has_content_length && body_seen < info.content_length) {
                errno = ECONNRESET;
                goto failure;
            }
            break;
        }
        if (send_all_until(client_fd, chunk, (size_t)received,
                   &client_write_deadline) != 0)
            goto failure;
        *response_bytes += (size_t)received;
        body_seen += (size_t)received;
        if (cache_eligible && !capture_disabled) {
            int appended = append_capture(captured, captured_len, &capture_capacity,
                                          chunk, (size_t)received,
                                          config->max_cached_response);
            if (appended != 0) {
                free(*captured);
                *captured = NULL;
                *captured_len = 0;
                capture_disabled = 1;
            }
        }
    }
    if (!headers_ready || (info.has_content_length && body_seen != info.content_length)) {
        errno = EPROTO;
        goto failure;
    }
    if (capture_disabled) {
        free(*captured);
        *captured = NULL;
        *captured_len = 0;
    }
    return 0;

failure:
    free(*captured);
    *captured = NULL;
    *captured_len = 0;
    return -1;
}

static void worker_finished(void)
{
    sem_post(&worker_slots);
    pthread_mutex_lock(&active_lock);
    --active_workers;
    pthread_cond_broadcast(&active_condition);
    pthread_mutex_unlock(&active_lock);
}

static void *handle_client(void *opaque)
{
    worker_arg_t *arg = opaque;
    const proxy_server_config_t *config = arg->config;
    int client_fd = arg->client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    memcpy(client_ip, arg->client_ip, sizeof(client_ip));
    free(arg);

    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    char request_buffer[BUFFER_SIZE];
    size_t request_length = 0;
    int extra_request_data = 0;
    int status = 400;
    int success = 0;
    int cache_hit = 0;
    size_t response_bytes = 0;
    http_request_t request;
    memset(&request, 0, sizeof(request));
    strcpy(request.method, "-");
    strcpy(request.host, "-");
    strcpy(request.path, "-");

    io_deadline_t header_deadline;
    if (set_socket_timeouts(client_fd, CLIENT_READ_TIMEOUT_SECONDS,
                            CLIENT_WRITE_DEADLINE_MS / 1000) != 0 ||
        deadline_after_ms(&header_deadline, CLIENT_HEADER_DEADLINE_MS) != 0) {
        status = 503;
        int sent = send_http_error(client_fd, 503, "Service Unavailable",
                                   "client socket setup failed\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    if (read_request_headers(client_fd, config->max_request_header,
                             request_buffer, &request_length,
                             &extra_request_data, &header_deadline) != 0 ||
        parse_http_request(request_buffer, &request) != 0) {
        int sent = send_http_error(client_fd, 400, "Bad Request", "malformed request\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    (void)request_length;
    if (extra_request_data || request_has_unsupported_body(request_buffer)) {
        status = 400;
        int sent = send_http_error(client_fd, 400, "Bad Request",
                                   "request bodies are unsupported\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    if (strcmp(request.method, "GET") != 0) {
        status = 400;
        int sent = send_http_error(client_fd, 400, "Bad Request", "only GET is supported\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    if (access_control_check(config->access_control, &request) != ACCESS_ALLOW) {
        status = 403;
        int sent = send_http_error(client_fd, 403, "Forbidden", "destination blocked\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }

    char cache_key[sizeof(request.host) + sizeof(request.path) + 32];
    if (make_cache_key(&request, cache_key, sizeof(cache_key)) != 0) {
        status = 400;
        int sent = send_http_error(client_fd, 400, "Bad Request", "invalid request target\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    unsigned char *cached_response = NULL;
    size_t cached_length = 0;
    int lookup = config->cache == NULL ? 0 :
                 cache_lookup(config->cache, cache_key, &cached_response, &cached_length);
    if (lookup < 0) {
        status = 502;
        int sent = send_http_error(client_fd, 502, "Bad Gateway", "cache lookup failed\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    if (lookup == 1) {
        cache_hit = 1;
        response_bytes = cached_length;
        if (cached_length >= 12)
            sscanf((char *)cached_response, "HTTP/%*s %d", &status);
        success = send_all_timeout(client_fd, cached_response, cached_length,
                       CLIENT_WRITE_DEADLINE_MS) == 0;
        free(cached_response);
        if (!success)
            status = 502;
        goto finished;
    }

    int origin_fd = connect_to_origin(request.host, request.port);
    if (origin_fd < 0) {
        status = errno == ETIMEDOUT ? 504 : 502;
        int sent = send_http_error(client_fd, status,
                       status == 504 ? "Gateway Timeout" : "Bad Gateway",
                       status == 504 ? "origin connection timed out\n" :
                               "origin connection failed\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    if (set_socket_timeouts(origin_fd, ORIGIN_RESPONSE_TIMEOUT_SECONDS,
                            ORIGIN_WRITE_DEADLINE_MS / 1000) != 0) {
        close(origin_fd);
        status = 502;
        int sent = send_http_error(client_fd, 502, "Bad Gateway",
                                   "origin socket setup failed\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    char authority[sizeof(request.host) + 16];
    int authority_length = snprintf(authority, sizeof(authority), "%s:%d",
                                    request.host, request.port);
    char origin_request[BUFFER_SIZE];
    int origin_request_length = snprintf(origin_request, sizeof(origin_request),
                                         "GET %s HTTP/1.1\r\nHost: %s\r\n"
                                         "Connection: close\r\n\r\n",
                                         request.path, authority);
    if (authority_length < 0 || (size_t)authority_length >= sizeof(authority) ||
        origin_request_length < 0 || (size_t)origin_request_length >= sizeof(origin_request) ||
        send_all_timeout(origin_fd, origin_request,
                 (size_t)origin_request_length,
                 ORIGIN_WRITE_DEADLINE_MS) != 0) {
        close(origin_fd);
        status = 502;
        int sent = send_http_error(client_fd, 502, "Bad Gateway", "origin request failed\n");
        if (sent > 0) response_bytes = (size_t)sent;
        goto finished;
    }
    unsigned char *captured = NULL;
    size_t captured_length = 0;
    if (forward_origin_response(origin_fd, client_fd, config, &status,
                                &response_bytes, &captured, &captured_length) == 0) {
        success = status >= 200 && status < 300;
        if (config->cache != NULL && captured != NULL)
            cache_store(config->cache, cache_key, captured, captured_length);
    } else {
        if (response_bytes == 0) {
            status = is_timeout_error(errno) ? 504 : 502;
            int sent = send_http_error(client_fd, status,
                                       status == 504 ? "Gateway Timeout" : "Bad Gateway",
                                       "origin response failed\n");
            if (sent > 0) response_bytes = (size_t)sent;
        }
    }
    free(captured);
    close(origin_fd);

finished:
    record_proxy_request(config, client_ip, request.method, request.host,
                         request.path, status, success, cache_hit,
                         response_bytes, &started);
    close(client_fd);
    worker_finished();
    return NULL;
}

int proxy_server_run(const proxy_server_config_t *config)
{
    if (config == NULL || config->access_control == NULL ||
        config->metrics == NULL || config->listen_port < 1 || config->listen_port > 65535) {
        errno = EINVAL;
        return -1;
    }
    if (config->max_request_header == 0 ||
        config->max_request_header > BUFFER_SIZE - 1 || config->max_workers > 10000) {
        errno = EINVAL;
        return -1;
    }
    int listener = create_server_socket(config->listen_port);
    if (listener < 0)
        return -1;
    stopping = 0;
    size_t max_workers = config->max_workers == 0 ? DEFAULT_WORKERS : config->max_workers;
    if (sem_init(&worker_slots, 0, (unsigned int)max_workers) != 0) {
        close(listener);
        return -1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    struct sigaction old_int, old_term;
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);

    while (!stopping) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        int client_fd = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (client_fd < 0) {
            if (errno == EINTR && !stopping)
                continue;
            if (stopping || errno == EBADF || errno == EINVAL)
                break;
            continue;
        }
        struct timespec accepted_at;
        clock_gettime(CLOCK_MONOTONIC, &accepted_at);
        if (sem_trywait(&worker_slots) != 0) {
            int sent = send_http_error(client_fd, 503, "Service Unavailable",
                                       "worker capacity reached\n");
            record_rejected_client(config, &peer, peer_length, 503, sent, &accepted_at);
            close(client_fd);
            continue;
        }
        worker_arg_t *arg = calloc(1, sizeof(*arg));
        if (arg == NULL) {
            sem_post(&worker_slots);
            int sent = send_http_error(client_fd, 503, "Service Unavailable",
                                       "worker allocation failed\n");
            record_rejected_client(config, &peer, peer_length, 503, sent, &accepted_at);
            close(client_fd);
            continue;
        }
        arg->config = config;
        arg->client_fd = client_fd;
        if (getnameinfo((struct sockaddr *)&peer, peer_length, arg->client_ip,
                        sizeof(arg->client_ip), NULL, 0, NI_NUMERICHOST) != 0)
            strcpy(arg->client_ip, "unknown");
        pthread_mutex_lock(&active_lock);
        ++active_workers;
        pthread_mutex_unlock(&active_lock);
        pthread_t thread;
        int thread_error = pthread_create(&thread, NULL, handle_client, arg);
        if (thread_error != 0) {
            pthread_mutex_lock(&active_lock);
            --active_workers;
            pthread_cond_broadcast(&active_condition);
            pthread_mutex_unlock(&active_lock);
            sem_post(&worker_slots);
            free(arg);
            int sent = send_http_error(client_fd, 503, "Service Unavailable",
                                       "worker creation failed\n");
            record_rejected_client(config, &peer, peer_length, 503, sent, &accepted_at);
            close(client_fd);
            continue;
        }
        int detach_error = pthread_detach(thread);
        if (detach_error != 0) {
            fprintf(stderr, "pthread_detach failed: %s; joining worker\n",
                strerror(detach_error));
            int join_error = pthread_join(thread, NULL);
            if (join_error != 0)
            fprintf(stderr, "pthread_join after detach failure: %s\n",
                strerror(join_error));
        }
    }

    close(listener);
    pthread_mutex_lock(&active_lock);
    while (active_workers != 0)
        pthread_cond_wait(&active_condition, &active_lock);
    pthread_mutex_unlock(&active_lock);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sem_destroy(&worker_slots);
    return 0;
}