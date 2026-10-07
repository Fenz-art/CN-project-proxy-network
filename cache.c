#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct cache_entry {
    char *key;
    unsigned char *response;
    size_t response_len;
    struct timespec expires_at;
    struct cache_entry *next;
} cache_entry_t;

struct cache {
    pthread_mutex_t lock;
    cache_entry_t *entries;
    size_t count;
    size_t max_entries;
    size_t max_response_size;
    unsigned ttl_seconds;
};

static int is_expired(const cache_entry_t *entry, const struct timespec *now)
{
    return entry->expires_at.tv_sec < now->tv_sec ||
           (entry->expires_at.tv_sec == now->tv_sec &&
            entry->expires_at.tv_nsec <= now->tv_nsec);
}

static void free_entry(cache_entry_t *entry)
{
    free(entry->key);
    free(entry->response);
    free(entry);
}

cache_t *cache_create(size_t max_entries, size_t max_response_size,
                      unsigned ttl_seconds)
{
    if (max_entries == 0 || max_response_size == 0 || ttl_seconds == 0)
        return NULL;
    cache_t *cache = calloc(1, sizeof(*cache));
    if (cache == NULL)
        return NULL;
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        free(cache);
        return NULL;
    }
    cache->max_entries = max_entries;
    cache->max_response_size = max_response_size;
    cache->ttl_seconds = ttl_seconds;
    return cache;
}

void cache_clear(cache_t *cache)
{
    if (cache == NULL)
        return;
    pthread_mutex_lock(&cache->lock);
    cache_entry_t *entry = cache->entries;
    cache->entries = NULL;
    cache->count = 0;
    pthread_mutex_unlock(&cache->lock);
    while (entry != NULL) {
        cache_entry_t *next = entry->next;
        free_entry(entry);
        entry = next;
    }
}

void cache_destroy(cache_t *cache)
{
    if (cache == NULL)
        return;
    cache_clear(cache);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

int cache_lookup(cache_t *cache, const char *key,
                 unsigned char **response, size_t *response_len)
{
    if (cache == NULL || key == NULL || response == NULL || response_len == NULL)
        return -1;
    *response = NULL;
    *response_len = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;

    pthread_mutex_lock(&cache->lock);
    cache_entry_t **cursor = &cache->entries;
    while (*cursor != NULL) {
        cache_entry_t *entry = *cursor;
        if (is_expired(entry, &now)) {
            *cursor = entry->next;
            --cache->count;
            free_entry(entry);
            continue;
        }
        if (strcmp(entry->key, key) == 0) {
            unsigned char *copy = malloc(entry->response_len);
            if (copy == NULL) {
                pthread_mutex_unlock(&cache->lock);
                return -1;
            }
            memcpy(copy, entry->response, entry->response_len);
            *response = copy;
            *response_len = entry->response_len;
            pthread_mutex_unlock(&cache->lock);
            return 1;
        }
        cursor = &entry->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

int cache_store(cache_t *cache, const char *key,
                const unsigned char *response, size_t response_len)
{
    if (cache == NULL || key == NULL || response == NULL || response_len == 0)
        return -1;
    if (response_len > cache->max_response_size)
        return 1;

    cache_entry_t *fresh = calloc(1, sizeof(*fresh));
    if (fresh == NULL)
        return -1;
    fresh->key = strdup(key);
    fresh->response = malloc(response_len);
    if (fresh->key == NULL || fresh->response == NULL) {
        free_entry(fresh);
        return -1;
    }
    memcpy(fresh->response, response, response_len);
    fresh->response_len = response_len;
    if (clock_gettime(CLOCK_MONOTONIC, &fresh->expires_at) != 0) {
        free_entry(fresh);
        return -1;
    }
    fresh->expires_at.tv_sec += cache->ttl_seconds;

    pthread_mutex_lock(&cache->lock);
    cache_entry_t **cursor = &cache->entries;
    while (*cursor != NULL && strcmp((*cursor)->key, key) != 0)
        cursor = &(*cursor)->next;
    if (*cursor != NULL) {
        cache_entry_t *old = *cursor;
        fresh->next = old->next;
        *cursor = fresh;
        free_entry(old);
        pthread_mutex_unlock(&cache->lock);
        return 0;
    }
    if (cache->count == cache->max_entries) {
        cache_entry_t **last = &cache->entries;
        while ((*last)->next != NULL)
            last = &(*last)->next;
        cache_entry_t *evicted = *last;
        *last = NULL;
        free_entry(evicted);
        --cache->count;
    }
    fresh->next = cache->entries;
    cache->entries = fresh;
    ++cache->count;
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

void cache_remove(cache_t *cache, const char *key)
{
    if (cache == NULL || key == NULL)
        return;
    pthread_mutex_lock(&cache->lock);
    cache_entry_t **cursor = &cache->entries;
    while (*cursor != NULL) {
        if (strcmp((*cursor)->key, key) == 0) {
            cache_entry_t *removed = *cursor;
            *cursor = removed->next;
            --cache->count;
            free_entry(removed);
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&cache->lock);
}