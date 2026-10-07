#define _POSIX_C_SOURCE 200809L
#include "http_tcp_handler.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>

static int parse_authority(const char *text, size_t length, char *host,
                           size_t host_size, int *port)
{
    size_t host_length = length;
    const char *colon = memchr(text, ':', length);

    if (length == 0 || memchr(text, '@', length) != NULL ||
        memchr(text, '[', length) != NULL || memchr(text, ']', length) != NULL)
        return -1;
    if (colon != NULL) {
        host_length = (size_t)(colon - text);
        if (host_length == 0 || host_length + 1 >= length)
            return -1;
        int parsed_port = 0;
        for (const char *digit = colon + 1; digit < text + length; ++digit) {
            if (!isdigit((unsigned char)*digit))
                return -1;
            if (parsed_port > (65535 - (*digit - '0')) / 10)
                return -1;
            parsed_port = parsed_port * 10 + (*digit - '0');
        }
        if (parsed_port < 1 || parsed_port > 65535)
            return -1;
        *port = parsed_port;
    }
    if (host_length == 0 || host_length >= host_size)
        return -1;
    if (memchr(text, ':', host_length) != NULL)
        return -1;

    size_t label_start = 0;
    for (size_t i = 0; i < host_length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '.') {
            if (i == label_start || text[i - 1] == '-')
                return -1;
            label_start = i + 1;
        } else if (!isalnum(ch) && ch != '-') {
            return -1;
        } else if (i == label_start && ch == '-') {
            return -1;
        }
    }
    if (text[host_length - 1] == '-' ||
        (label_start == host_length && text[host_length - 1] != '.'))
        return -1;
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    return 0;
}

static int parse_request_target(char *target, http_request_t *req,
                                int *absolute_form)
{
    char authority[BUFFER_SIZE];
    char *resource = target;
    int target_port = 80;
    *absolute_form = 0;

    if (strncasecmp(target, "http://", 7) == 0) {
        *absolute_form = 1;
        char *authority_start = target + 7;
        char *resource_start = strpbrk(authority_start, "/?#");
        size_t authority_length = resource_start == NULL
                                      ? strlen(authority_start)
                                      : (size_t)(resource_start - authority_start);
        if (authority_length == 0 || authority_length >= sizeof(authority))
            return -1;
        memcpy(authority, authority_start, authority_length);
        authority[authority_length] = '\0';
        if (parse_authority(authority, authority_length, req->host,
                            sizeof(req->host), &target_port) != 0)
            return -1;
        if (resource_start == NULL) {
            resource = "/";
        } else if (*resource_start == '?') {
            memmove(resource_start + 1, resource_start,
                    strlen(resource_start) + 1);
            resource_start[0] = '/';
            resource = resource_start;
        } else {
            resource = resource_start;
        }
    } else if (strstr(target, "://") != NULL) {
        return -1;
    }

    if (*resource != '/' || strchr(resource, '#') != NULL ||
        strlen(resource) >= sizeof(req->path))
        return -1;
    memcpy(req->path, resource, strlen(resource) + 1);
    req->port = target_port;
    return 0;
}

int create_server_socket(int port)
{
    if (port < 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return -1;

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listen_fd);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 10) < 0) {
        int saved_errno = errno;
        close(listen_fd);
        errno = saved_errno;
        return -1;
    }
    return listen_fd;
}

int parse_http_request(const char *buffer, http_request_t *req)
{
    if (req == NULL)
        return -1;
    memset(req, 0, sizeof(*req));
    if (buffer == NULL)
        return -1;

    size_t request_length = strnlen(buffer, BUFFER_SIZE);
    if (request_length == 0 || request_length == BUFFER_SIZE)
        return -1;
    memcpy(req->raw_request, buffer, request_length + 1);

    char request_copy[BUFFER_SIZE];
    memcpy(request_copy, buffer, request_length + 1);
    char *line_end = strstr(request_copy, "\r\n");
    if (line_end == NULL)
        return -1;
    *line_end = '\0';
    char *first_space = strchr(request_copy, ' ');
    if (first_space == NULL || first_space == request_copy)
        return -1;
    char *second_space = strchr(first_space + 1, ' ');
    if (second_space == NULL || second_space == first_space + 1 ||
        strchr(second_space + 1, ' ') != NULL)
        return -1;
    size_t method_length = (size_t)(first_space - request_copy);
    size_t target_length = (size_t)(second_space - first_space - 1);
    if (method_length >= sizeof(req->method) || target_length == 0 ||
        target_length >= 2048)
        return -1;
    for (size_t i = 0; i < method_length; ++i) {
        unsigned char ch = (unsigned char)request_copy[i];
        if (!isalnum(ch) && strchr("!#$%&'*+-.^_`|~", ch) == NULL)
            return -1;
    }
    if (strcmp(second_space + 1, "HTTP/1.0") != 0 &&
        strcmp(second_space + 1, "HTTP/1.1") != 0)
        return -1;
    memcpy(req->method, request_copy, method_length);
    req->method[method_length] = '\0';

    char target[2048];
    memcpy(target, first_space + 1, target_length);
    target[target_length] = '\0';
    for (size_t i = 0; i < target_length; ++i) {
        unsigned char ch = (unsigned char)target[i];
        if (ch <= 0x20 || ch >= 0x7f)
            return -1;
    }
    int absolute_form;
    if (parse_request_target(target, req, &absolute_form) != 0)
        return -1;

    int host_seen = 0;
    int header_host_port = 80;
    char header_host[sizeof(req->host)] = {0};
    char *cursor = line_end + 2;
    char *headers_end = request_copy + request_length;
    int ended_headers = 0;
    while (cursor < headers_end) {
        char *end = strstr(cursor, "\r\n");
        if (end == NULL)
            return -1;
        if (end == cursor) {
            ended_headers = 1;
            cursor = end + 2;
            break;
        }
        *end = '\0';
        if (*cursor == ' ' || *cursor == '\t')
            return -1;
        char *colon = strchr(cursor, ':');
        if (colon == NULL || colon == cursor)
            return -1;
        for (char *ch = cursor; ch < colon; ++ch) {
            if (!isalnum((unsigned char)*ch) && strchr("!#$%&'*+-.^_`|~", *ch) == NULL)
                return -1;
        }
        if ((size_t)(colon - cursor) == 4 && strncasecmp(cursor, "Host", 4) == 0) {
            if (host_seen)
                return -1;
            host_seen = 1;
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                ++value;
            char *value_end = end;
            while (value_end > value &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t'))
                --value_end;
            if (parse_authority(value, (size_t)(value_end - value), header_host,
                                sizeof(header_host), &header_host_port) != 0)
                return -1;
        }
        cursor = end + 2;
    }
    if (!ended_headers || (strcmp(second_space + 1, "HTTP/1.1") == 0 && !host_seen))
        return -1;
    if (!absolute_form && !host_seen)
        return -1;
    if (!absolute_form) {
        memcpy(req->host, header_host, strlen(header_host) + 1);
        req->port = header_host_port;
    } else if (host_seen &&
               (strcasecmp(req->host, header_host) != 0 || req->port != header_host_port)) {
        return -1;
    }
    return 0;
}

static int remaining_timeout_ms(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    int64_t remaining = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
                        (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (remaining <= 0)
        return 0;
    return remaining > INT32_MAX ? INT32_MAX : (int)remaining;
}

int connect_to_origin_with_timeout(const char *host, int port, int timeout_ms)
{
    if (host == NULL || *host == '\0' || port < 1 || port > 65535 || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    char service[6];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int lookup = getaddrinfo(host, service, &hints, &addresses);
    if (lookup != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    int result_fd = -1;
    int last_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        int original_flags = fcntl(fd, F_GETFL, 0);
        if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
            last_errno = errno;
            close(fd);
            continue;
        }
        int connected = connect(fd, address->ai_addr, address->ai_addrlen);
        if (connected < 0 && errno == EINPROGRESS) {
            struct pollfd poll_fd = { .fd = fd, .events = POLLOUT };
            int ready;
            do {
                int remaining = remaining_timeout_ms(&deadline);
                if (timeout_ms == 0 || remaining == 0) {
                    ready = 0;
                    break;
                }
                ready = poll(&poll_fd, 1, remaining);
            } while (ready < 0 && errno == EINTR);
            if (ready > 0) {
                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 &&
                    socket_error == 0)
                    connected = 0;
                else {
                    last_errno = socket_error != 0 ? socket_error : errno;
                    connected = -1;
                }
            } else {
                last_errno = ready == 0 ? ETIMEDOUT : errno;
                connected = -1;
            }
        } else if (connected < 0) {
            last_errno = errno;
        }
        if (connected == 0 && fcntl(fd, F_SETFL, original_flags) == 0) {
            result_fd = fd;
            break;
        }
        if (connected == 0)
            last_errno = errno;
        close(fd);
        if (last_errno == ETIMEDOUT)
            break;
    }
    freeaddrinfo(addresses);
    if (result_fd < 0)
        errno = last_errno;
    return result_fd;
}

int connect_to_origin(const char *host, int port)
{
    return connect_to_origin_with_timeout(host, port, ORIGIN_CONNECT_TIMEOUT_MS);
}

int relay_data(int src_fd, int dest_fd)
{
    char buffer[BUFFER_SIZE];
    for (;;) {
        ssize_t bytes_read = recv(src_fd, buffer, sizeof(buffer), 0);
        if (bytes_read == 0)
            return 0;
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        size_t offset = 0;
        while (offset < (size_t)bytes_read) {
#ifdef MSG_NOSIGNAL
            ssize_t bytes_sent = send(dest_fd, buffer + offset,
                                      (size_t)bytes_read - offset, MSG_NOSIGNAL);
#else
            ssize_t bytes_sent = send(dest_fd, buffer + offset,
                                      (size_t)bytes_read - offset, 0);
#endif
            if (bytes_sent < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (bytes_sent == 0) {
                errno = EPIPE;
                return -1;
            }
            offset += (size_t)bytes_sent;
        }
    }
}