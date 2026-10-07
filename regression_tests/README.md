# Regression Suite

Run all checks with:

```sh
make test
```

The runner builds tests in a temporary directory and does not add generated test
artifacts to the source tree. It executes:

- Existing HTTP parser, connection, relay, metrics, and logger regressions.
- Cache unit tests for exact response copies, key isolation, size, expiration,
  eviction, clear/remove, and concurrent access.
- Access-control unit tests for case-insensitive names, default policies,
  comments, malformed rules, duplicates, and BLOCK precedence.
- A real local-origin/proxy integration test for forwarding, both HTTP request
  forms, cache HIT/MISS/expiry/eviction inputs, authority/port/path/query
  isolation, access-control BLOCK, errors, chunked passthrough, large bodies,
  disconnect recovery, 1/2/5/10/25/50 concurrency, and worker-capacity 503s.
- Three repetitions each of direct and real-proxy benchmarks at 1/5/10/25/50
  clients, with caching disabled and the same local origin/path.

The integration tests prove cache HIT and blocked-request behavior using origin
request counters. Benchmark numbers are environment-dependent localhost
measurements and should not be generalized to external networks.
