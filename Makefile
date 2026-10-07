CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
THREAD_FLAGS = -pthread
BUILD_DIR = build

PROXY_SOURCES = main.c proxy_server.c cache.c access_control.c \
	http_tcp_handler.c logger.c metrics.c

.PHONY: all clean test regression performance

all: $(BUILD_DIR)/proxy $(BUILD_DIR)/performance_test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/proxy: $(PROXY_SOURCES) http_tcp_handler.h logger.h metrics.h \
		cache.h access_control.h proxy_server.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(PROXY_SOURCES) -o $@

$(BUILD_DIR)/performance_test: performance_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $< -o $@

$(BUILD_DIR)/local_origin_server: tests/fixtures/local_origin_server.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $< -o $@

regression:
	sh regression_tests/run.sh

test: regression

performance: $(BUILD_DIR)/performance_test
	@echo 'Run direct:  build/performance_test direct <origin_host> <origin_port> <path> <clients>'
	@echo 'Run proxy:   build/performance_test proxy <proxy_host> <proxy_port> <origin_host> <origin_port> <path> <clients>'
	@echo 'Proxy-mode measurements require a running proxy and origin.'

clean:
	rm -rf $(BUILD_DIR)