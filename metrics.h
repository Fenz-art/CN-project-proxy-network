#ifndef METRICS_H
#define METRICS_H

#include <pthread.h>

typedef struct
{
    long total_requests;
    long successful_requests;
    long failed_requests;
    long cache_hits;
    long cache_misses;
    long total_bytes;
    double total_latency;
    pthread_mutex_t lock;
} Metrics;

/* Initialize before sharing across threads and destroy after all users stop. */
void init_metrics(Metrics *m);
void destroy_metrics(Metrics *m);

void record_request(
    Metrics *m,
    int success,
    int cache_hit,
    long response_size,
    double latency
);

void print_metrics(Metrics *m);
void save_metrics(Metrics *m);

#endif

