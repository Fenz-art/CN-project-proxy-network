#define _POSIX_C_SOURCE 200809L

#include "access_control.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *hostname;
    int block;
} access_rule_t;

struct access_control {
    access_rule_t *rules;
    size_t count;
    size_t capacity;
    int default_allow;
};

static int valid_hostname(const char *hostname)
{
    size_t length = strlen(hostname);

    if (length == 0 || length > 253 ||
        hostname[0] == '.' || hostname[length - 1] == '.')
        return 0;

    size_t label_length = 0;

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)hostname[i];

        if (ch == '.') {
            if (label_length == 0 || hostname[i - 1] == '-')
                return 0;

            label_length = 0;
        } else if (isalnum(ch) || ch == '-') {
            if (label_length == 0 && ch == '-')
                return 0;

            ++label_length;

            if (label_length > 63)
                return 0;
        } else {
            return 0;
        }
    }

    return label_length != 0 && hostname[length - 1] != '-';
}

access_control_t *access_control_create(int default_allow)
{
    access_control_t *control = calloc(1, sizeof(*control));

    if (control != NULL)
        control->default_allow = default_allow != 0;

    return control;
}

void access_control_destroy(access_control_t *control)
{
    if (control == NULL)
        return;

    for (size_t i = 0; i < control->count; ++i)
        free(control->rules[i].hostname);

    free(control->rules);
    free(control);
}

static int add_rule(access_control_t *control,
                    const char *hostname,
                    int block)
{
    for (size_t i = 0; i < control->count; ++i) {
        if (strcasecmp(control->rules[i].hostname, hostname) == 0) {
            if (block)
                control->rules[i].block = 1;

            return 0;
        }
    }

    if (control->count == control->capacity) {
        size_t capacity =
            control->capacity == 0 ? 8 : control->capacity * 2;

        access_rule_t *rules =
            realloc(control->rules, capacity * sizeof(*rules));

        if (rules == NULL)
            return -1;

        control->rules = rules;
        control->capacity = capacity;
    }

    char *copy = strdup(hostname);

    if (copy == NULL)
        return -1;

    for (char *ch = copy; *ch != '\0'; ++ch)
        *ch = (char)tolower((unsigned char)*ch);

    control->rules[control->count++] =
        (access_rule_t){ .hostname = copy, .block = block };

    return 0;
}

int access_control_load_file(access_control_t *control,
                             const char *path)
{
    if (control == NULL || path == NULL)
        return -1;

    FILE *file = fopen(path, "r");

    if (file == NULL)
        return -1;

    char line[512];
    int result = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            result = -1;
            break;
        }

        char *comment = strchr(line, '#');

        if (comment != NULL)
            *comment = '\0';

        char *save = NULL;

        char *action =
            strtok_r(line, " \t\r\n", &save);

        if (action == NULL)
            continue;

        char *hostname =
            strtok_r(NULL, " \t\r\n", &save);

        char *extra =
            strtok_r(NULL, " \t\r\n", &save);

        int block;

        if (hostname == NULL ||
            extra != NULL ||
            (strcmp(action, "allow") != 0 &&
             strcmp(action, "block") != 0) ||
            !valid_hostname(hostname)) {

            result = -1;
            break;
        }

        block = strcmp(action, "block") == 0;

        if (add_rule(control, hostname, block) != 0) {
            result = -1;
            break;
        }
    }

    if (ferror(file))
        result = -1;

    fclose(file);

    return result;
}

access_decision_t access_control_check(
    const access_control_t *control,
    const http_request_t *request)
{
    if (control == NULL ||
        request == NULL ||
        request->host[0] == '\0')
        return ACCESS_ERROR;

    size_t request_host_length = strlen(request->host);

    if (request_host_length > 1 &&
        request->host[request_host_length - 1] == '.')
        --request_host_length;

    int explicit_allow = 0;

    for (size_t i = 0; i < control->count; ++i) {
        size_t rule_host_length =
            strlen(control->rules[i].hostname);

        if (rule_host_length == request_host_length &&
            strncasecmp(control->rules[i].hostname,
                        request->host,
                        request_host_length) == 0) {

            if (control->rules[i].block)
                return ACCESS_BLOCK;

            explicit_allow = 1;
        }
    }

    if (explicit_allow || control->default_allow)
        return ACCESS_ALLOW;

    return ACCESS_BLOCK;
}