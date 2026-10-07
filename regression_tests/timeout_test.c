#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "http_tcp_handler.h"

static int attempted_fd = -1;
static int observed_timeout_ms = -1;

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    (void)address;
    (void)length;
    attempted_fd = fd;
    errno = EINPROGRESS;
    return -1;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
    (void)fds;
    (void)count;
    observed_timeout_ms = timeout_ms;
    return 0;
}

int main(void)
{
    int result = connect_to_origin_with_timeout("127.0.0.1", 80, 100);
    int saved_errno = errno;
    int descriptor_closed = attempted_fd >= 0 &&
                            fcntl(attempted_fd, F_GETFD) == -1 && errno == EBADF;
    if (result >= 0)
        close(result);
    if (result != -1 || saved_errno != ETIMEDOUT ||
        observed_timeout_ms < 1 || observed_timeout_ms > 100 || !descriptor_closed) {
        fprintf(stderr,
                "timeout test failed: fd=%d result=%d errno=%d poll_ms=%d closed=%d\n",
                attempted_fd, result, saved_errno, observed_timeout_ms,
                descriptor_closed);
        return 1;
    }
    puts("PASS forced connect timeout reports ETIMEDOUT and closes socket");
    return 0;
}
