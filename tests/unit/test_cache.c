#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static void check(int ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

typedef struct { cache_t *cache; int id; int ok; } worker_arg_t;
static void *cache_worker(void *opaque)
{
    worker_arg_t *arg = opaque;
    char key[64], value[64];
    snprintf(key, sizeof(key), "host:%d/item", arg->id);
    snprintf(value, sizeof(value), "value-%d", arg->id);
    arg->ok = cache_store(arg->cache, key, (const unsigned char *)value,
                          strlen(value)) == 0;
    unsigned char *copy = NULL;
    size_t length = 0;
    arg->ok = arg->ok && cache_lookup(arg->cache, key, &copy, &length) == 1 &&
              length == strlen(value) && memcmp(copy, value, length) == 0;
    free(copy);
    return NULL;
}

int main(void)
{
    cache_t *cache = cache_create(2, 128, 1);
    check(cache != NULL, "cache create");
    const unsigned char first[] = "first";
    const unsigned char second[] = "second";
    check(cache_store(cache, "host:80/a?q=1", first, sizeof(first)) == 0,
          "cache store");
    unsigned char *copy = NULL;
    size_t length = 0;
    check(cache_lookup(cache, "host:80/a?q=1", &copy, &length) == 1 &&
          length == sizeof(first) && memcmp(copy, first, length) == 0,
          "cache lookup returns an owned exact response copy");
    free(copy);
    check(cache_lookup(cache, "other:80/a?q=1", &copy, &length) == 0,
          "authority participates in cache key");
    check(cache_lookup(cache, "host:80/a?q=2", &copy, &length) == 0,
          "query participates in cache key");
    check(cache_store(cache, "host:80/large", first, 129) == 1,
          "oversized response is not stored");
    cache_store(cache, "host:80/b", second, sizeof(second));
    cache_store(cache, "host:80/c", second, sizeof(second));
    check(cache_lookup(cache, "host:80/a?q=1", &copy, &length) == 0,
          "capacity evicts oldest entry");
    check(cache_lookup(cache, "host:80/c", &copy, &length) == 1,
          "newest entry remains after eviction");
    free(copy);

      cache_t *concurrent_cache = cache_create(32, 128, 5);
      pthread_t threads[16];
    worker_arg_t args[16];
    for (int i = 0; i < 16; ++i) {
            args[i] = (worker_arg_t){ .cache = concurrent_cache, .id = i, .ok = 0 };
        pthread_create(&threads[i], NULL, cache_worker, &args[i]);
    }
    int all_ok = 1;
    for (int i = 0; i < 16; ++i) {
        pthread_join(threads[i], NULL);
        all_ok &= args[i].ok;
    }
    check(all_ok, "concurrent cache stores and lookups");
      cache_destroy(concurrent_cache);

    struct timespec pause = { .tv_sec = 1, .tv_nsec = 100000000L };
    nanosleep(&pause, NULL);
    check(cache_lookup(cache, "host:80/c", &copy, &length) == 0,
          "expired entry is a miss");
    cache_remove(cache, "host:80/b");
    cache_clear(cache);
    check(cache_lookup(cache, "host:80/c", &copy, &length) == 0,
          "clear removes all entries");
    cache_destroy(cache);
    puts("Cache unit test complete");
    return failures == 0 ? 0 : 1;
}
