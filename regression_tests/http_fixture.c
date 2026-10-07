#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, data + offset, length - offset, 0);
        if (sent <= 0)
            return -1;
        offset += (size_t)sent;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    char *end = NULL;
    long requests = strtol(argv[1], &end, 10);
    if (*end != '\0' || requests < 1 || requests > 1000)
        return 2;
    int expect_absolute = strcmp(argv[2], "proxy") == 0;
    int truncated_response = strcmp(argv[2], "truncated") == 0;
    int short_body = strcmp(argv[2], "short-body") == 0;
    if (!expect_absolute && !truncated_response && !short_body &&
        strcmp(argv[2], "direct") != 0)
        return 2;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return 1;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    socklen_t address_size = sizeof(address);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, (int)requests) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_size) != 0) {
        close(listener);
        return 1;
    }
    printf("%u\n", (unsigned)ntohs(address.sin_port));
    fflush(stdout);

    int valid = 1;
    for (long request_index = 0; request_index < requests; ++request_index) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            valid = 0;
            break;
        }
        char request[4096];
        size_t length = 0;
        while (length + 1 < sizeof(request)) {
            ssize_t received = recv(client, request + length,
                                    sizeof(request) - length - 1, 0);
            if (received <= 0) {
                valid = 0;
                break;
            }
            length += (size_t)received;
            request[length] = '\0';
            if (strstr(request, "\r\n\r\n") != NULL)
                break;
        }
        if (length == 0 || strstr(request, "\r\n\r\n") == NULL ||
            (expect_absolute && strncmp(request, "GET http://127.0.0.1:1/", 23) != 0) ||
            (!expect_absolute && strncmp(request, "GET / HTTP/1.1\r\n", 16) != 0))
            valid = 0;
        static const char valid_response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
            "Connection: close\r\n\r\nhello";
        static const char partial_response[] = "HTTP/1.1 200 OK\r\n";
        static const char incomplete_body_response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
            "Connection: close\r\n\r\nhe";
        const char *response = truncated_response ? partial_response :
                               short_body ? incomplete_body_response : valid_response;
        size_t response_length = truncated_response ? sizeof(partial_response) - 1 :
                                 short_body ? sizeof(incomplete_body_response) - 1 :
                                 sizeof(valid_response) - 1;
        if (write_all(client, response, response_length) != 0)
            valid = 0;
        close(client);
    }
    close(listener);
    return valid ? 0 : 1;
}