#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include "http_tcp_handler.h"

typedef struct access_control access_control_t;

typedef enum {
    ACCESS_ERROR = -1,
    ACCESS_ALLOW = 0,
    ACCESS_BLOCK = 1
} access_decision_t;

access_control_t *access_control_create(int default_allow);
void access_control_destroy(access_control_t *control);
int access_control_load_file(access_control_t *control, const char *path);
access_decision_t access_control_check(const access_control_t *control,
                                       const http_request_t *request);

#endif