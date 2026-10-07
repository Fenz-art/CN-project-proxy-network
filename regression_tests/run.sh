#!/bin/sh
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR="$ROOT/regression_tests"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
FAILURES=0

report() {
    status=$1
    name=$2
    printf '%-6s %s\n' "$status" "$name"
    if [ "$status" = FAIL ]; then
        FAILURES=$((FAILURES + 1))
    fi
}

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -I"$ROOT" \
    "$TEST_DIR/contract_tests.c" "$ROOT/http_tcp_handler.c" \
    "$ROOT/metrics.c" "$ROOT/logger.c" -pthread -o "$TMP/contract_tests" \
    >"$TMP/build.log" 2>&1; then
    (cd "$TMP" && ./contract_tests)
    result=$?
    if [ "$result" -eq 0 ]; then report PASS "parser/transport/relay/logger/metrics contracts"; \
    else report FAIL "parser/transport/relay/logger/metrics contracts (exit $result)"; fi
else
    report FAIL "compile contract tests"
    sed -n '1,20p' "$TMP/build.log"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -I"$ROOT" \
    "$TEST_DIR/timeout_test.c" "$ROOT/http_tcp_handler.c" \
    -Wl,--wrap=connect -Wl,--wrap=poll -o "$TMP/timeout_test" \
    >"$TMP/timeout-build.log" 2>&1; then
    if "$TMP/timeout_test"; then report PASS "forced origin connect timeout and descriptor cleanup"; \
    else report FAIL "forced origin connect timeout"; fi
else
    report FAIL "compile origin timeout test"
    sed -n '1,20p' "$TMP/timeout-build.log"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
    "$ROOT/test_module.c" "$ROOT/logger.c" "$ROOT/metrics.c" \
    -pthread -o "$TMP/test_module" >"$TMP/unit-build.log" 2>&1; then
    (cd "$TMP" && ./test_module >"$TMP/test-module.out")
    result=$?
    if [ "$result" -eq 0 ]; then report PASS "supplied test_module build/run"; \
    else report FAIL "supplied test_module run"; fi
else
    report FAIL "supplied test_module build"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
    "$ROOT/performance_test.c" -pthread -o "$TMP/performance_test" \
    >"$TMP/performance-build.log" 2>&1; then
    report PASS "performance_test build"
    if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$TEST_DIR/http_fixture.c" -o "$TMP/http_fixture" \
        >"$TMP/fixture-build.log" 2>&1; then
        mkfifo "$TMP/direct.fifo"
        "$TMP/http_fixture" 4 direct >"$TMP/direct.fifo" &
        direct_fixture_pid=$!
        IFS= read -r direct_port < "$TMP/direct.fifo"
        if direct_output=$("$TMP/performance_test" direct 127.0.0.1 \
            "$direct_port" / 4); then
            printf '%s\n' "$direct_output"
            case "$direct_output" in
                *"Successful       : 4"*) report PASS "direct benchmark localhost smoke" ;;
                *) report FAIL "direct benchmark localhost success accounting" ;;
            esac
        else
            printf '%s\n' "$direct_output"
            report FAIL "direct benchmark localhost smoke"
        fi
        wait "$direct_fixture_pid" || report FAIL "direct HTTP fixture validation"

        mkfifo "$TMP/proxy.fifo"
        "$TMP/http_fixture" 4 proxy >"$TMP/proxy.fifo" &
        proxy_fixture_pid=$!
        IFS= read -r proxy_port < "$TMP/proxy.fifo"
        if proxy_output=$("$TMP/performance_test" proxy 127.0.0.1 \
            "$proxy_port" 127.0.0.1 1 / 4); then
            printf '%s\n' "$proxy_output"
            case "$proxy_output" in
                *"Successful       : 4"*) report PASS "proxy-mode absolute-form localhost smoke (fixture only)" ;;
                *) report FAIL "proxy-mode success accounting" ;;
            esac
        else
            printf '%s\n' "$proxy_output"
            report FAIL "proxy-mode localhost protocol smoke"
        fi
        wait "$proxy_fixture_pid" || report FAIL "proxy-mode request-form fixture validation"

        mkfifo "$TMP/truncated.fifo"
        "$TMP/http_fixture" 1 truncated >"$TMP/truncated.fifo" &
        truncated_fixture_pid=$!
        IFS= read -r truncated_port < "$TMP/truncated.fifo"
        if truncated_output=$("$TMP/performance_test" direct 127.0.0.1 \
            "$truncated_port" / 1); then
            printf '%s\n' "$truncated_output"
            report FAIL "truncated HTTP response rejected by benchmark"
        else
            printf '%s\n' "$truncated_output"
            case "$truncated_output" in
                *"Successful       : 0"*"Failed           : 1"*) report PASS "truncated HTTP response counted as failure" ;;
                *) report FAIL "truncated response failure accounting" ;;
            esac
        fi
        wait "$truncated_fixture_pid" || report FAIL "truncated HTTP fixture validation"

        mkfifo "$TMP/short-body.fifo"
        "$TMP/http_fixture" 1 short-body >"$TMP/short-body.fifo" &
        short_body_fixture_pid=$!
        IFS= read -r short_body_port < "$TMP/short-body.fifo"
        if short_body_output=$("$TMP/performance_test" direct 127.0.0.1 \
            "$short_body_port" / 1); then
            printf '%s\n' "$short_body_output"
            report FAIL "incomplete Content-Length body rejected by benchmark"
        else
            printf '%s\n' "$short_body_output"
            case "$short_body_output" in
                *"Successful       : 0"*"Failed           : 1"*) report PASS "incomplete Content-Length body counted as failure" ;;
                *) report FAIL "incomplete body failure accounting" ;;
            esac
        fi
        wait "$short_body_fixture_pid" || report FAIL "short-body HTTP fixture validation"

        if "$TMP/performance_test" direct 127.0.0.1 not-a-port / 1 \
            >"$TMP/invalid-cli.out" 2>&1; then
            report FAIL "benchmark rejects invalid port argument"
        else
            report PASS "benchmark rejects invalid port argument"
        fi
    else
        report FAIL "compile local HTTP fixture"
        sed -n '1,20p' "$TMP/fixture-build.log"
    fi
else
    report FAIL "performance_test build"
    sed -n '1,8p' "$TMP/performance-build.log"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread -I"$ROOT" \
    "$TEST_DIR/../tests/unit/test_cache.c" "$ROOT/cache.c" \
    -o "$TMP/test_cache" >"$TMP/cache-build.log" 2>&1; then
    if (cd "$TMP" && ./test_cache); then report PASS "cache unit tests"; \
    else report FAIL "cache unit tests"; fi
else
    report FAIL "cache unit test build"
    sed -n '1,20p' "$TMP/cache-build.log"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread -I"$ROOT" \
    "$TEST_DIR/../tests/unit/test_access_control.c" "$ROOT/access_control.c" \
    -o "$TMP/test_access_control" >"$TMP/access-build.log" 2>&1; then
    if (cd "$TMP" && ./test_access_control); then report PASS "access-control unit tests"; \
    else report FAIL "access-control unit tests"; fi
else
    report FAIL "access-control unit test build"
    sed -n '1,20p' "$TMP/access-build.log"
fi

if gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread -I"$ROOT" \
    "$ROOT/main.c" "$ROOT/proxy_server.c" "$ROOT/cache.c" \
    "$ROOT/access_control.c" "$ROOT/http_tcp_handler.c" \
    "$ROOT/logger.c" "$ROOT/metrics.c" -o "$TMP/proxy" \
    >"$TMP/proxy-build.log" 2>&1 && \
   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
    "$TEST_DIR/../tests/fixtures/local_origin_server.c" \
    -o "$TMP/local_origin" >"$TMP/origin-build.log" 2>&1 && \
   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
    "$TEST_DIR/../tests/integration/test_proxy.c" \
    -o "$TMP/test_proxy" >"$TMP/integration-build.log" 2>&1; then
    if (cd "$TMP" && ./test_proxy "$TMP/proxy" "$TMP/local_origin" \
        "$ROOT/config/access_control.conf" "$TMP/performance_test"); then
        report PASS "real proxy/origin integration and concurrency"
    else
        report FAIL "real proxy/origin integration and concurrency"
    fi
else
    report FAIL "proxy/origin/integration build"
    for log in "$TMP/proxy-build.log" "$TMP/origin-build.log" "$TMP/integration-build.log"; do
        if [ -f "$log" ]; then sed -n '1,20p' "$log"; fi
    done
fi

printf 'Failures: %s\n' "$FAILURES"
[ "$FAILURES" -eq 0 ]