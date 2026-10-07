#include <stdio.h>
#include "metrics.h"

typedef struct
{
    long total_requests;
    long successful_requests;
    long failed_requests;
    long cache_hits;
    long cache_misses;
    long total_bytes;
    double total_latency;
} MetricsSnapshot;

void init_metrics(Metrics *m)
{
    if (m == NULL)
        return;
    m->total_requests = 0;
    m->successful_requests = 0;
    m->failed_requests = 0;
    m->cache_hits = 0;
    m->cache_misses = 0;
    m->total_bytes = 0;
    m->total_latency = 0.0;
    int error = pthread_mutex_init(&m->lock, NULL);
    if (error != 0)
        fprintf(stderr, "Unable to initialize metrics lock: %d\n", error);
}

void destroy_metrics(Metrics *m)
{
    if (m != NULL)
        pthread_mutex_destroy(&m->lock);
}

void record_request(Metrics *m, int success, int cache_hit,
                    long response_size, double latency)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->lock);
    m->total_requests++;

    if (success)
        m->successful_requests++;
    else
        m->failed_requests++;

    if (cache_hit)
        m->cache_hits++;
    else
        m->cache_misses++;

    m->total_bytes += response_size;
    m->total_latency += latency;
    pthread_mutex_unlock(&m->lock);
}

void print_metrics(Metrics *m)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->lock);
    MetricsSnapshot snapshot = {
        .total_requests = m->total_requests,
        .successful_requests = m->successful_requests,
        .failed_requests = m->failed_requests,
        .cache_hits = m->cache_hits,
        .cache_misses = m->cache_misses,
        .total_bytes = m->total_bytes,
        .total_latency = m->total_latency
    };
    pthread_mutex_unlock(&m->lock);

    double average_latency = 0.0;
    double cache_hit_rate = 0.0;

    if (snapshot.total_requests > 0)
    {
        average_latency = snapshot.total_latency / snapshot.total_requests;
        cache_hit_rate =
            ((double)snapshot.cache_hits / snapshot.total_requests) * 100.0;
    }

    printf("\n========== PROXY METRICS ==========\n");
    printf("Total Requests     : %ld\n", snapshot.total_requests);
    printf("Successful         : %ld\n", snapshot.successful_requests);
    printf("Failed             : %ld\n", snapshot.failed_requests);
    printf("Cache Hits         : %ld\n", snapshot.cache_hits);
    printf("Cache Misses       : %ld\n", snapshot.cache_misses);
    printf("Cache Hit Rate     : %.2f%%\n", cache_hit_rate);
    printf("Total Data         : %ld bytes\n", snapshot.total_bytes);
    printf("Average Latency    : %.2f ms\n", average_latency);
    printf("===================================\n");
}

void save_metrics(Metrics *m)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->lock);
    MetricsSnapshot snapshot = {
        .total_requests = m->total_requests,
        .successful_requests = m->successful_requests,
        .failed_requests = m->failed_requests,
        .cache_hits = m->cache_hits,
        .cache_misses = m->cache_misses,
        .total_bytes = m->total_bytes,
        .total_latency = m->total_latency
    };
    pthread_mutex_unlock(&m->lock);

    FILE *fp = fopen("metrics.csv", "w");

    if (fp == NULL)
    {
        perror("Unable to create metrics.csv");
        return;
    }

    double average_latency = 0.0;

    if (snapshot.total_requests > 0)
        average_latency = snapshot.total_latency / snapshot.total_requests;

    fprintf(fp,
            "Total Requests,Successful,Failed,Cache Hits,"
            "Cache Misses,Total Bytes,Average Latency(ms)\n");

    fprintf(fp, "%ld,%ld,%ld,%ld,%ld,%ld,%.2f\n",
            snapshot.total_requests, snapshot.successful_requests,
            snapshot.failed_requests, snapshot.cache_hits, snapshot.cache_misses,
            snapshot.total_bytes, average_latency);

    fclose(fp);
}

