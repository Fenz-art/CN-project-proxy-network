#include <stdio.h>
#include "logger.h"
#include "metrics.h"

int main()
{
    Metrics metrics;

    init_logger();
    init_metrics(&metrics);

    /* Test 1: Successful request - Cache MISS */
    log_request("127.0.0.1", "GET", "example.com",
                "/index.html", 200, 0, 15432, 182.50);
    record_request(&metrics, 1, 0, 15432, 182.50);

    /* Test 2: Successful request - Cache HIT */
    log_request("127.0.0.1", "GET", "example.com",
                "/index.html", 200, 1, 15432, 4.20);
    record_request(&metrics, 1, 1, 15432, 4.20);

    /* Test 3: Failed request */
    log_request("127.0.0.1", "GET", "invalid-server",
                "/", 502, 0, 0, 1000.40);
    record_request(&metrics, 0, 0, 0, 1000.40);

    print_metrics(&metrics);
    save_metrics(&metrics);
    close_logger();
    destroy_metrics(&metrics);

    return 0;
}

