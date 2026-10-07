#define _POSIX_C_SOURCE 200809L
#include "access_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

static int write_rules(char *path, const char *contents)
{
    strcpy(path, "access-rules-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    size_t length = strlen(contents);
    ssize_t written = write(fd, contents, length);
    close(fd);
    return written == (ssize_t)length ? 0 : -1;
}

int main(void)
{
    char path[64];
    check(write_rules(path,
          "\n# comment\nallow Example.com\nallow duplicate.test\n"
          "block duplicate.test\nblock blocked.test\n") == 0,
          "create valid rule fixture with comments and blank lines");
    access_control_t *control = access_control_create(0);
    check(control != NULL && access_control_load_file(control, path) == 0,
          "load rules with default-deny policy");
    access_control_t *default_allow_control = access_control_create(1);
    check(default_allow_control != NULL &&
          access_control_load_file(default_allow_control, path) == 0,
          "load same block rules with default-allow policy");
    unlink(path);
    http_request_t request = {0};
    strcpy(request.host, "example.COM");
    check(access_control_check(control, &request) == ACCESS_ALLOW,
          "hostname rules are case-insensitive");
    strcpy(request.host, "blocked.test");
    check(access_control_check(control, &request) == ACCESS_BLOCK,
          "explicit block denies hostname");
    strcpy(request.host, "blocked.test.");
      check(access_control_check(default_allow_control, &request) == ACCESS_BLOCK,
          "terminal-dot hostname cannot bypass a block rule");
    strcpy(request.host, "duplicate.test");
      check(access_control_check(default_allow_control, &request) == ACCESS_BLOCK,
          "block rule wins over duplicate allow rule");
    strcpy(request.host, "unknown.test");
    check(access_control_check(control, &request) == ACCESS_BLOCK,
          "default-deny policy blocks unknown hostname");
    access_control_destroy(control);
      access_control_destroy(default_allow_control);

    check(write_rules(path, "allow\n") == 0, "create malformed rules fixture");
    control = access_control_create(1);
    check(access_control_load_file(control, path) == -1,
          "missing hostname rejects malformed rule file");
    access_control_destroy(control);
    unlink(path);
    control = access_control_create(1);
check(control != NULL &&
      access_control_load_file(control, path) == -1,
      "missing config file returns error");
access_control_destroy(control);
    control = access_control_create(1);
    strcpy(request.host, "unknown.test");
    check(access_control_check(control, &request) == ACCESS_ALLOW,
          "default-allow policy allows unknown hostname");
    check(access_control_check(control, NULL) == ACCESS_ERROR,
          "invalid check arguments return access error");
    access_control_destroy(control);
    return failures == 0 ? 0 : 1;
}
