#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include "logger.h"

static FILE *log_file = NULL;
static pthread_mutex_t logger_lock = PTHREAD_MUTEX_INITIALIZER;

void init_logger()
{
    pthread_mutex_lock(&logger_lock);
    FILE *opened = fopen("proxy.log", "a");

    if (opened == NULL)
    {
        perror("Unable to open proxy.log");
        pthread_mutex_unlock(&logger_lock);
        return;
    }
    if (log_file != NULL)
        fclose(log_file);
    log_file = opened;
    pthread_mutex_unlock(&logger_lock);
}

void log_request(
    const char *client_ip,
    const char *method,
    const char *host,
    const char *url,
    int status_code,
    int cache_hit,
    long response_size,
    double latency)
{
    pthread_mutex_lock(&logger_lock);
    if (log_file == NULL)
    {
        pthread_mutex_unlock(&logger_lock);
        return;
    }

    time_t now = time(NULL);
    struct tm timestamp;
    if (localtime_r(&now, &timestamp) == NULL)
    {
        pthread_mutex_unlock(&logger_lock);
        return;
    }

    fprintf(log_file,
            "[%02d-%02d-%04d %02d:%02d:%02d] "
            "Client=%s | Method=%s | Host=%s | URL=%s | "
            "Status=%d | Cache=%s | Size=%ld bytes | "
            "Latency=%.2f ms\n",
            timestamp.tm_mday, timestamp.tm_mon + 1, timestamp.tm_year + 1900,
            timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec,
            client_ip, method, host, url, status_code,
            cache_hit ? "HIT" : "MISS", response_size, latency);

    fflush(log_file);
    pthread_mutex_unlock(&logger_lock);
}

void close_logger()
{
    pthread_mutex_lock(&logger_lock);
    if (log_file != NULL)
    {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&logger_lock);
}

