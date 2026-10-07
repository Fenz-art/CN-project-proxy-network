#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_LIMIT 16384
#define LARGE_BODY_SIZE (1024 * 1024)
#define OVERSIZED_BODY_SIZE (1024 * 1024 + 1)

typedef struct request_count {
    char path[2048];
    unsigned long count;
    struct request_count *next;
} request_count_t;

typedef struct {
    int fd;
} client_arg_t;

static volatile sig_atomic_t stopping;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_condition = PTHREAD_COND_INITIALIZER;
static size_t active_clients;
static request_count_t *counts;
static unsigned long total_requests;

static void stop_server(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int send_all(int fd, const void *data, size_t length)
{
    const char *bytes = data;
    size_t sent_total = 0;
    while (sent_total < length) {
#ifdef MSG_NOSIGNAL
        ssize_t sent = send(fd, bytes + sent_total, length - sent_total, MSG_NOSIGNAL);
#else
        ssize_t sent = send(fd, bytes + sent_total, length - sent_total, 0);
#endif
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (sent == 0)
            return -1;
        sent_total += (size_t)sent;
    }
    return 0;
}

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const void *body,
                         size_t body_length)
{
    char header[512];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          status, reason, content_type, body_length);
    if (length < 0 || (size_t)length >= sizeof(header) ||
        send_all(fd, header, (size_t)length) != 0)
        return -1;
    return body_length == 0 ? 0 : send_all(fd, body, body_length);
}

static unsigned long count_for_path(const char *path)
{
    unsigned long value = 0;
    pthread_mutex_lock(&count_lock);
    for (request_count_t *entry = counts; entry != NULL; entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            value = entry->count;
            break;
        }
    }
    pthread_mutex_unlock(&count_lock);
    return value;
}

static void record_path(const char *path)
{
    pthread_mutex_lock(&count_lock);
    ++total_requests;
    request_count_t *entry = counts;
    while (entry != NULL && strcmp(entry->path, path) != 0)
        entry = entry->next;
    if (entry == NULL) {
        entry = calloc(1, sizeof(*entry));
        if (entry != NULL) {
            snprintf(entry->path, sizeof(entry->path), "%s", path);
            entry->next = counts;
            counts = entry;
        }
    }
    if (entry != NULL)
        ++entry->count;
    pthread_mutex_unlock(&count_lock);
}

static void client_finished(void)
{
    pthread_mutex_lock(&clients_lock);
    --active_clients;
    pthread_cond_broadcast(&clients_condition);
    pthread_mutex_unlock(&clients_lock);
}

static void *serve_client(void *opaque)
{
    client_arg_t *arg = opaque;
    int fd = arg->fd;
    free(arg);
    char request[REQUEST_LIMIT + 1];
    request[0] = '\0';
    size_t used = 0;
    while (used < REQUEST_LIMIT) {
        ssize_t received = recv(fd, request + used, REQUEST_LIMIT - used, 0);
        if (received <= 0)
            break;
        used += (size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL)
            break;
    }
    char method[16] = {0};
    char path[2048] = {0};
    if (sscanf(request, "%15s %2047s", method, path) != 2 ||
        strcmp(method, "GET") != 0) {
        send_response(fd, 400, "Bad Request", "text/plain", "bad request\n", 12);
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/_count") == 0 || strncmp(path, "/_count?", 8) == 0) {
        const char *requested_path = strstr(path, "path=");
        char count_body[32];
        unsigned long value;
        if (requested_path == NULL) {
            pthread_mutex_lock(&count_lock);
            value = total_requests;
            pthread_mutex_unlock(&count_lock);
        } else {
            char count_path[2048];
            size_t count_path_length = strcspn(requested_path + 5, "&");
            if (count_path_length >= sizeof(count_path))
                count_path_length = sizeof(count_path) - 1;
            memcpy(count_path, requested_path + 5, count_path_length);
            count_path[count_path_length] = '\0';
            value = count_for_path(count_path);
        }
        int length = snprintf(count_body, sizeof(count_body), "%lu\n", value);
        send_response(fd, 200, "OK", "text/plain", count_body,
                      length > 0 ? (size_t)length : 0);
        close(fd);
        client_finished();
        return NULL;
    }

    record_path(path);
    if (strcmp(path, "/chunked") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/chunked-with-length") == 0) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            "4\r\nWiki\r\n0\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/delay") == 0) {
        struct timespec pause = { .tv_sec = 9, .tv_nsec = 0 };
        nanosleep(&pause, NULL);
    }
    if (strcmp(path, "/trickle") == 0) {
        static const char headers[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n"
            "Connection: close\r\n\r\n";
        send_all(fd, headers, sizeof(headers) - 1);
        for (int i = 0; i < 12; ++i) {
            struct timespec pause = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&pause, NULL);
            if (send_all(fd, "x", 1) != 0)
                break;
        }
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/status") == 0) {
        static const char body[] = "not found\n";
        send_response(fd, 404, "Not Found", "text/plain", body, sizeof(body) - 1);
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/large") == 0) {
        unsigned char *body = malloc(LARGE_BODY_SIZE);
        if (body != NULL) {
            for (size_t i = 0; i < LARGE_BODY_SIZE; ++i)
                body[i] = (unsigned char)('a' + (i % 26));
            send_response(fd, 200, "OK", "application/octet-stream",
                          body, LARGE_BODY_SIZE);
            free(body);
        }
        close(fd);
        client_finished();
        return NULL;
    }
    if (strcmp(path, "/too-large") == 0) {
        unsigned char *body = malloc(OVERSIZED_BODY_SIZE);
        if (body != NULL) {
            memset(body, 'z', OVERSIZED_BODY_SIZE);
            send_response(fd, 200, "OK", "application/octet-stream",
                          body, OVERSIZED_BODY_SIZE);
            free(body);
        }
        close(fd);
        client_finished();
        return NULL;
    }

    char body[4096];
    int body_length;
    if (strncmp(path, "/query", 6) == 0 || strcmp(path, "/cache") == 0 ||
        strcmp(path, "/different") == 0) {
        body_length = snprintf(body, sizeof(body), "origin:%s\n", path);
    } else if (strcmp(path, "/hello") == 0) {
        body_length = snprintf(body, sizeof(body), "hello from local origin\n");
    } else {
        body_length = snprintf(body, sizeof(body), "origin:%s\n", path);
    }
    send_response(fd, 200, "OK", "text/plain", body,
                  body_length > 0 ? (size_t)body_length : 0);
    close(fd);
    client_finished();
    return NULL;
}

int main(int argc, char **argv)
{
    int port = 0;
    if (argc > 2 || (argc == 2 &&
        (sscanf(argv[1], "%d", &port) != 1 || port < 0 || port > 65535))) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0)
        return 1;
    int reuse = 1;
    setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener_fd, 128) != 0 ||
        getsockname(listener_fd, (struct sockaddr *)&address, &address_length) != 0) {
        perror("origin fixture setup");
        close(listener_fd);
        return 1;
    }
    printf("%u\n", (unsigned)ntohs(address.sin_port));
    fflush(stdout);

    while (!stopping) {
        struct pollfd listener_event = { .fd = listener_fd, .events = POLLIN };
        int ready = poll(&listener_event, 1, 250);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR && !stopping)
                continue;
            if (stopping)
                break;
            continue;
        }
        client_arg_t *arg = malloc(sizeof(*arg));
        if (arg == NULL) {
            close(client_fd);
            continue;
        }
        arg->fd = client_fd;
        pthread_t thread;
        pthread_mutex_lock(&clients_lock);
        ++active_clients;
        pthread_mutex_unlock(&clients_lock);
        if (pthread_create(&thread, NULL, serve_client, arg) != 0) {
            close(client_fd);
            free(arg);
            pthread_mutex_lock(&clients_lock);
            --active_clients;
            pthread_cond_broadcast(&clients_condition);
            pthread_mutex_unlock(&clients_lock);
            continue;
        }
        pthread_detach(thread);
    }
    close(listener_fd);
    pthread_mutex_lock(&clients_lock);
    while (active_clients != 0)
        pthread_cond_wait(&clients_condition, &clients_lock);
    pthread_mutex_unlock(&clients_lock);
    pthread_mutex_lock(&count_lock);
    request_count_t *entry = counts;
    counts = NULL;
    pthread_mutex_unlock(&count_lock);
    while (entry != NULL) {
        request_count_t *next = entry->next;
        free(entry);
        entry = next;
    }
    pthread_mutex_destroy(&count_lock);
    pthread_cond_destroy(&clients_condition);
    pthread_mutex_destroy(&clients_lock);
    return 0;
}