# Proxy Server Design and Evaluation

## Scope

This project implements a bounded educational HTTP forward proxy supporting
HTTP/1.0 and HTTP/1.1 GET traffic. HTTPS CONNECT tunneling, IPv6 authority
literals, request bodies, and non-GET methods are outside the implemented
scope.

The proxy is intended for the assignment's controlled HTTP demonstrations. It
is not a general-purpose browser/Internet proxy.

## Architecture

The listener binds to IPv4 loopback. It accepts a client connection and starts
one detached worker per request, with a semaphore limiting active workers to
64. Requests have a bounded header size and client-side deadlines. The worker
parses the HTTP request target and Host authority, checks hostname access
rules, and then looks up an eligible GET in the shared cache. A cache miss
connects to the origin, sends a new origin-form GET with Host and
Connection: close, and streams the response to the client. Access decisions,
cache results, status, byte count, and elapsed time are logged; aggregate
metrics are written on shutdown.

The listener, HTTP parser, cache, access-control module, logger, and metrics
are separate C modules. No external libraries are required beyond the C
compiler, POSIX threads, and system networking APIs.

## Design choices and trade-offs

- **Bounded thread-per-request concurrency:** straightforward ownership and
  isolation for this project; the fixed worker limit prevents unbounded thread
  growth. Excess clients receive HTTP 503 rather than blocking a worker slot.
- **Bounded in-memory cache:** defaults are 32 entries, 1 MiB per response,
  and a 30-second TTL. The key includes normalized hostname, port, path, and
  query. Oldest-inserted entries are evicted when full. Only successful
  responses with Content-Length are cached; chunked and larger responses are
  forwarded but not cached.
- **Educational cache semantics:** this cache does not interpret
  Cache-Control, Vary, or validators. The TTL and size/capacity bounds make
  the policy explicit, but it is not a standards-compliant shared HTTP cache.
- **Exact-host policy:** optional `allow`/`block` rules compare hostnames
  case-insensitively. Explicit block takes precedence; unlisted hosts are
  allowed by default. Rules run before cache lookup and origin connection.
- **Bounded network waits:** header, origin-response, and write deadlines
  bound slow-client and slow-origin cases. System name resolution itself is
  not separately deadline-bounded.
- **Loopback-only listener:** avoids exposing an unauthenticated educational
  proxy to other machines by default.

## Requirement validation

`make test` runs parser/transport contracts, cache and access-control unit
tests, plus real local-origin/proxy integration checks. The integration checks
include forwarding, cache HIT/MISS/expiry proven by origin request counts,
BLOCK proven not to reach the origin, malformed input, unreachable and delayed
origins, chunked and large response forwarding, disconnect recovery, and
concurrency at 1, 2, 5, 10, 25, and 50 clients. A synchronized 65-client test
checks the 64-worker capacity response. The same integration run benchmarks
direct and real-proxy requests at 1, 5, 10, 25, and 50 clients, with proxy
caching disabled.

Validation on 8 October 2026:

```text
make clean       PASS
make             PASS, no compiler warnings
make test        PASS, Failures: 0, Integration failures: 0
GCC -fanalyzer   PASS, no diagnostics on production C sources
```

Clang, Cppcheck, and Valgrind were unavailable in the validation environment.

## Performance results

The figures below are the latest three-repetition arithmetic means printed by
`make test`. Direct clients connect to the local origin; proxy-mode clients
connect through the proxy to that same origin. Cache is disabled for the
comparison.

| Clients | Direct latency | Proxied latency | Observed overhead | Direct req/s | Proxied req/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.590 ms | 1.120 ms | 89.83% | 1,316.3 | 790.9 |
| 5 | 0.297 ms | 0.437 ms | 47.19% | 11,642.7 | 7,546.2 |
| 10 | 0.177 ms | 0.357 ms | 101.89% | 21,913.7 | 15,413.7 |
| 25 | 0.183 ms | 0.483 ms | 163.64% | 30,021.4 | 20,086.5 |
| 50 | 0.253 ms | 14.227 ms | 5,515.79% | 26,479.5 | 11,950.1 |

These are localhost, environment-dependent microbenchmarks and should not be
generalized to external networks. The 50-client proxied latency is a clear
outlier in this run; retain it in the report rather than treating it as a
repeatable performance level. The benchmark is a short comparison, not a
statistically rigorous study. Cache hit behavior is tested separately and is
not part of these direct-versus-proxy measurements.

## Team responsibilities

The member/responsibility mapping in the README is the team's stated primary
assignment of work. Responsibilities overlap during integration and testing;
the mapping is not a claim that each module was implemented exclusively by
one person.

## Reproducible demonstration

Follow [DEMO_CHECKLIST.md](DEMO_CHECKLIST.md) to run the bundled loopback
origin and demonstrate normal forwarding, cache miss/hit/expiry, and hostname
blocking without requiring Internet access.
