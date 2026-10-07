#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

typedef struct cache cache_t;

cache_t *cache_create(size_t max_entries, size_t max_response_size,
                      unsigned ttl_seconds);
void cache_destroy(cache_t *cache);

/* Returns 1 on HIT, 0 on MISS, or -1 on error. The caller owns *response. */
int cache_lookup(cache_t *cache, const char *key,
                 unsigned char **response, size_t *response_len);
/* Returns 0 when stored, 1 when ineligible/too large, or -1 on error. */
int cache_store(cache_t *cache, const char *key,
                const unsigned char *response, size_t response_len);
void cache_remove(cache_t *cache, const char *key);
void cache_clear(cache_t *cache);

#endif