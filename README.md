# Computer Networks — Mini Project

## Project 1: Designing a Proxy Server

## Implemented Baseline

The current implementation is a concurrent HTTP forward proxy built from the
existing flat source layout. Build it with `make`; run `build/proxy --help` for
options. The proxy accepts HTTP/1.0 and HTTP/1.1 `GET` requests in absolute-form
or origin-form, converts requests to origin-form, applies hostname policy, then
checks the response cache before contacting the origin. Request headers are
bounded to 8 KiB and workers are capped at 64. The listener binds to IPv4
loopback by default. Client headers have a 5-second total deadline; origin
response forwarding has an 8-second total deadline; each also has an inactivity
socket timeout. Client and origin writes have bounded 5-second deadlines.
Origin TCP connect defaults to 5 seconds. Name resolution still uses the system
resolver and is not independently timed.

The cache uses a lowercased `host:port/path?query` key, a 30-second default TTL,
32 entries, and a 1 MiB maximum response by default. It stores successful GET
responses with a valid `Content-Length` only. Capacity eviction removes the
oldest inserted entry. This is an educational cache, not RFC-complete HTTP
caching; it does not interpret `Cache-Control`, `Vary`, or validators. Chunked
responses are forwarded as their original byte stream and are not cached.

Access rules are exact hostnames, case-insensitive; a single trailing DNS root
dot is normalized before rule comparison. `block` wins over `allow`;
the default is allow for unlisted hosts. The sample policy blocks
`blocked.test`. HTTPS tunneling, IPv6 authorities, request bodies, and methods
other than GET are unsupported.

The proxy is a lab implementation without client authentication. Its default
loopback binding avoids LAN/public exposure. Do not change the bind address or
expose it outside a controlled network without adding appropriate client access
controls.

Run all unit, legacy regression, and local end-to-end tests with `make test`.
The concise final project report is in [REPORT.md](REPORT.md).
The test suite launches the real proxy and local origin, verifies cache hits and
access blocks through origin counters, and exercises concurrency through the
validated local regression set. It also runs direct and real-proxy benchmark
repetitions at 1, 5, 10, 25, and 50 clients against the same local origin,
with proxy caching disabled for the comparison. Results are printed by the
integration suite and are environment-specific; no fixed performance claim is
made beyond the observed localhost measurements above.

## FINAL VALIDATION

The feature-frozen implementation was validated with:

```bash
make clean && make && make test
```

Observed result from the repository's real regression run:

```text
Failures: 0
Integration failures: 0
```

Additional hardening validation was also executed against the running proxy:

- slow/partial client headers were rejected and followed by a valid request
- the proxy recovered after a timed-out request path
- a 250-client burst completed with `CONCURRENCY_COUNT=250` and `ERR_COUNT=0`
- the HTTP parser and cache/ACL regressions were verified under the real local origin
- timeout and worker-capacity behavior remained stable under repeated use

This is a validated service-level recovery check, not a claim that 250 clients were concurrently active workers. The proxy is capped at 64 workers and rejects excess traffic with HTTP 503.

> The following sections retain the team's original structure, updated to
> describe the implemented system. The implementation scope and limitations at
> the beginning of this README are authoritative.

> **Course:** Computer Networks
> **Project Type:** Mini Project
> **Team Size:** 4 Members
> **Final Submission Deadline:** 11 October 2026

---

## 👥 Team Members

| # | Name              | Roll Number | Primary Responsibility                             |
| - | ----------------- | ----------- | -------------------------------------------------- |
| 1 | **Kumar Kushang** | `2405734`   | Core Architecture, Concurrency & Caching           |
| 2 | **Tonisha Ghosh** | `24051896`  | TCP Connection Layer & HTTP Handling               |
| 3 | **Ahona Misra**   | `24051078`  | Access Control, Configuration & Error Handling     |
| 4 | **Aastha Ray**    | `24051975`  | Logging, Testing, Metrics & Performance Evaluation |

> Team members participated in integration, debugging, documentation, testing, and demonstration preparation. Each member is expected to understand the complete system and its design decisions.

---

# 1. Project Overview

We have selected **Project 1 — Designing a Proxy Server** for our Computer Networks mini-project.

The project implements a **concurrent HTTP forward proxy server** that acts as an intermediary between clients and destination web servers.

The proxy receives HTTP requests from clients, parses and processes them, applies access-control rules, checks its response cache, forwards misses to destination servers, returns responses to clients, and records traffic information.

The system handles simultaneous clients, rejects malformed requests, handles network failures, and measures the overhead introduced by the proxy.

The project specification describes the proxy as an intermediary that controls and optimizes how requests flow between clients and the Internet, while requiring realistic protocol handling rather than a simplified request format.

---

# 2. Why We Chose This Project

We chose this project because it provides an opportunity to apply several Computer Networks concepts together in a practical networking system.

The project brings together:

* TCP socket-based communication
* HTTP and application-layer protocols
* Client-server communication
* Concurrent connection handling
* Response caching
* Access-control mechanisms
* Traffic monitoring and logging
* Network error handling
* Latency and throughput measurement

The implementation goes beyond basic forwarding to demonstrate how a network intermediary can **inspect, control, optimize, and monitor traffic** between clients and destination servers.

The implementation demonstrates trade-offs involving concurrency, caching, access control, reliability, and network performance.

---

# 3. Project Objectives

The project objectives are:

1. Implement a working **HTTP forward proxy server**.
2. Correctly parse and forward HTTP requests.
3. Support multiple simultaneous client connections.
4. Implement response caching with a defined cache policy.
5. Distinguish between cache hits and cache misses.
6. Implement access control for selected domains or URLs.
7. Maintain useful traffic logs.
8. Handle malformed requests and unreachable destinations gracefully.
9. Measure latency and throughput with and without the proxy.
10. Analyze the performance overhead introduced by the proxy.

The implementation and test suite address these Project 1 objectives within the HTTP GET scope documented above.

---

# 4. Implemented System

The system functions as a **forward proxy**.

Instead of a client communicating directly with a destination server:

```text
Client ───────────────────────► Destination Server
```

the communication passes through the proxy:

```text
Client
   │
   │ HTTP Request
   ▼
┌─────────────────┐
│  Proxy Server   │
└────────┬────────┘
         │
         │ HTTP Request
         ▼
Destination Server
         │
         │ HTTP Response
         ▼
┌─────────────────┐
│  Proxy Server   │
└────────┬────────┘
         │
         │ HTTP Response
         ▼
       Client
```

The proxy is an intermediary capable of applying access policy, serving eligible cached responses, and forwarding requests to destination servers.

---

# 5. High-Level Architecture

```text
                           INTERNET
                              │
                              │
                     ┌────────▼────────┐
                     │  Origin Server  │
                     │ example.com etc.│
                     └────────▲────────┘
                              │
                         HTTP Response
                              │
                     ┌────────┴────────┐
                     │    Upstream     │
                     │ Connection      │
                     │    Manager      │
                     └────────▲────────┘
                              │
                        Cache MISS
                              │
┌──────────┐           ┌──────┴─────────────────────┐
│ Client A │──────────►│                             │
└──────────┘           │        PROXY SERVER         │
                       │                             │
┌──────────┐           │ ┌─────────────────────────┐ │
│ Client B │──────────►│ │   HTTP Request Parser   │ │
└──────────┘           │ └────────────┬────────────┘ │
                       │              │              │
┌──────────┐           │              ▼              │
│ Client C │──────────►│ ┌─────────────────────────┐ │
└──────────┘           │ │    Access Controller   │ │
                       │ └────────────┬────────────┘ │
                       │              │              │
                       │              ▼              │
                       │ ┌─────────────────────────┐ │
                       │ │     Cache Manager       │ │
                       │ └────────────┬────────────┘ │
                       │              │              │
                       │       ┌──────┴──────┐       │
                       │       │             │       │
                       │      HIT           MISS    │
                       │       │             │       │
                       │       ▼             └───────┼──► Upstream
                       │ Cached Response             │
                       │       │                     │
                       │       └─────────┐           │
                       │                 ▼           │
                       │ ┌─────────────────────────┐ │
                       │ │    Response Handler     │ │
                       │ └────────────┬────────────┘ │
                       │              │              │
                       │              ▼              │
                       │ ┌─────────────────────────┐ │
                       │ │     Traffic Logger      │ │
                       │ └─────────────────────────┘ │
                       └──────────────┬──────────────┘
                                      │
                                      ▼
                               Client Response
```

---

# 6. Core Features

## 6.1 HTTP Request Parsing and Forwarding

The proxy accepts HTTP requests from clients and parses the supported request information.

For example:

```http
GET /index.html HTTP/1.1
Host: example.com
Connection: close
```

The proxy extracts the method, destination, port, and resource, then establishes communication with the destination server.

The proxy forwards an origin-form request to the destination and returns the resulting response to the client.

The project requires handling a real protocol such as HTTP rather than relying on a simplified custom request format.

---

## 6.2 Concurrent Client Handling

The proxy supports multiple clients simultaneously, using a maximum of 64 detached workers. Additional accepted clients receive HTTP 503 while capacity is full.

Conceptually:

```text
                 Proxy Server
                      │
          ┌───────────┼───────────┐
          │           │           │
          ▼           ▼           ▼
       Client A    Client B    Client C
          │           │           │
          └───────────┼───────────┘
                  Concurrently
```

Each worker owns its client connection. Header, origin-response, and write deadlines bound common slow-peer cases so one client does not indefinitely occupy a worker.

The bounded worker model is exercised at 1, 2, 5, 10, 25, and 50 clients, plus a 65-client capacity test.

---

# 7. Response Caching

The proxy uses a bounded in-memory response cache for eligible GET responses.

The request flow is:

```text
                 HTTP Request
                      │
                      ▼
                Cache Lookup
                      │
              ┌───────┴───────┐
              │               │
             HIT             MISS
              │               │
              ▼               ▼
      Cached Response     Origin Server
              │               │
              │               ▼
              │          New Response
              │               │
              │          Store in Cache
              │               │
              └───────┬───────┘
                      ▼
                    Client
```

### Cache Hit

For a valid, unexpired cached response:

```text
Client
  │
  ▼
Proxy
  │
  ▼
Cache HIT
  │
  ▼
Cached Response
  │
  ▼
Client
```

The destination server does not need to be contacted for that request.

### Cache Miss

On a cache miss:

```text
Client
  │
  ▼
Proxy
  │
  ▼
Cache MISS
  │
  ▼
Destination Server
  │
  ▼
Response
  │
  ├────────► Client
  │
  └────────► Cache
```

Expired entries are treated as misses and fetched again from the origin. The cache policy is deliberately educational rather than RFC-complete.

---

# 8. Access Control

The proxy implements exact-hostname access control.

An optional text configuration file determines whether a request is allowed. Explicit block rules take precedence; unlisted destinations follow the default-allow policy.

```text
                Incoming Request
                       │
                       ▼
                Access Control
                       │
                ┌──────┴──────┐
                │             │
              ALLOW         BLOCK
                │             │
                ▼             ▼
           Continue        Reject
           Processing      Request
```

For example:

```text
Allowed:
example.org
wikipedia.org

Blocked:
blocked-domain.example
```

A request to a blocked destination is rejected with HTTP 403 before cache lookup or origin connection.

The implemented policy is limited to exact hostname rules; client-address restrictions and rate limiting are not implemented.

---

# 9. Traffic Logging

The proxy writes structured traffic logs for handled and rejected requests.

A log entry contains:

```text
Timestamp
Client
HTTP Method
Destination Host
Requested Path
Response Status
Cache Status
Response Size
Latency
```

Record format:

```text
[DD-MM-YYYY hh:mm:ss] Client=<address> | Method=<method> | Host=<host> | URL=<resource> | Status=<code> | Cache=<HIT/MISS> | Size=<bytes> | Latency=<ms>
```

This gives an operator request outcomes, cache decisions, response sizes, and latency for review.

---

# 10. Error Handling

The proxy handles the tested protocol and network failures without terminating the server.

The implementation handles cases such as:

* Malformed HTTP requests
* Invalid request information
* Unreachable destination servers
* Connection failures
* Network timeouts
* Client disconnections

Conceptually:

```text
Malformed Request
       │
       ▼
HTTP Parser
       │
       ▼
Reject / Error Response
```

and:

```text
Destination Unreachable
       │
       ▼
Connection Failure
       │
       ▼
Graceful Error Handling
       │
       ▼
Log Failure
       │
       ▼
Continue Serving Other Clients
```

Integration tests verify malformed input and unreachable destinations receive error responses and that the proxy continues serving subsequent clients.

---

# 11. Performance Evaluation

The integration benchmark measures the observed overhead introduced by the proxy.

It compares:

### Direct connection

```text
Client ─────────────────► Server
```

against:

### Proxied connection

```text
Client ─────► Proxy ─────► Server
```

The evaluation records direct and proxied latency, successful-request throughput,
and behavior at several concurrency levels. Cache behavior is validated separately.

The latest recorded run used three repetitions at each concurrency level:

| Clients | Direct latency | Real proxy latency | Observed overhead | Direct req/s | Proxy req/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.303 ms | 0.460 ms | 51.65% | 2,502.2 | 1,584.9 |
| 5 | 0.250 ms | 0.573 ms | 129.33% | 10,617.1 | 7,332.6 |
| 10 | 0.200 ms | 0.457 ms | 128.33% | 20,307.5 | 12,073.0 |
| 25 | 0.297 ms | 0.507 ms | 70.79% | 20,879.0 | 18,346.4 |
| 50 | 0.160 ms | 0.480 ms | 200.00% | 31,294.4 | 22,415.1 |

The direct path is client-to-local-origin; the proxy path is client-to-running-proxy-to-the-same-local-origin. Both use the same request path and response, with caching disabled for comparison. The benchmark counts successful responses after validating HTTP status and framing; separate integration tests verify deterministic and large response bodies byte-for-byte.

These are short localhost microbenchmarks. Scheduler activity, machine load, and run-to-run variation can materially affect the very small absolute latencies. They describe this test environment and are not statistically rigorous or generalizable to Internet-scale proxy performance. This comparison does not measure cache-hit speedups; cache-hit behavior is tested separately through origin counters.

The project specification requires latency/throughput measurements with and without the proxy; the table above records the local direct-versus-real-proxy comparison.

---

# 12. Demonstration Scenarios

The integration suite demonstrates the following scenarios against a controlled local origin.

### 12.1 Normal Forwarding

```text
Client
  │
  ▼
Proxy
  │
  ▼
Destination Server
  │
  ▼
Proxy
  │
  ▼
Client
```

---

### 12.2 Cache Miss

```text
Request
   │
   ▼
CACHE MISS
   │
   ▼
Destination Server
   │
   ▼
Response
   │
   ├──► Client
   └──► Cache
```

---

### 12.3 Cache Hit

```text
Same Request
     │
     ▼
 CACHE HIT
     │
     ▼
Cached Response
     │
     ▼
   Client
```

---

### 12.4 Access Control

```text
Client
  │
  ▼
Proxy
  │
  ▼
Blocked Domain
  │
  ▼
Request Rejected
```

---

### 12.5 Concurrent Clients

```text
Client A ─┐
Client B ─┼──► Proxy ───► Internet
Client C ─┘
```

Multiple requests are processed concurrently, up to the configured worker cap.

---

### 12.6 Failure Handling

An invalid or unreachable destination demonstrates that the proxy handles failures without crashing and continues serving later requests.

---

### 12.7 Performance Comparison

```text
Direct Connection
        VS
Proxy Connection
```

Latency and throughput results are recorded by direct and real-proxy benchmark modes.

These scenarios correspond to the required demonstration and evaluation areas of the project.

---

# 13. Work Distribution

Work was divided among the four members, each with a primary implementation responsibility and shared participation in integration and testing.

## Kumar Kushang — 2405734

### Core Architecture, Concurrency and Caching

Responsibilities:

* Overall proxy architecture
* Core request-processing pipeline
* Concurrency model
* Concurrent client handling
* Cache architecture
* Cache data structures
* Cache hit/miss logic
* Cache expiration/invalidation policy
* Integration of major proxy components
* Investigation of architectural trade-offs

---

## Tonisha Ghosh — 24051896

### TCP Connection Layer and HTTP Handling

Responsibilities:

* TCP socket listener
* Client connection handling
* Destination-server connections
* HTTP request parsing
* HTTP response handling
* Request/response forwarding
* Protocol-level testing
* HTTP-related edge cases

---

## Ahona Misra — 24051078

### Access Control, Configuration and Error Handling

Responsibilities:

* Domain/URL access-control mechanism
* Block/allow rules
* Configuration handling
* Request validation
* Timeout handling
* Connection-error handling
* Malformed-request handling
* Client/server disconnection handling
* Access-control and failure testing

---

## Aastha Ray — 24051975

### Logging, Metrics, Testing and Performance Evaluation

Responsibilities:

* Traffic logging system
* Request/response metrics
* Cache hit/miss statistics
* Latency measurement
* Throughput measurement
* Automated and manual test cases
* Concurrent-load testing
* Direct-vs-proxy performance comparison
* Performance analysis and presentation of results

---

### Shared Responsibilities

Although each member has a primary area of responsibility, all members participated in:

* System integration
* Debugging
* Code review
* Testing
* Documentation
* Final demonstration
* Understanding the complete architecture

Each member is expected to understand and explain their own implementation as well as the overall system flow.

---

# 14. AI Usage Declaration

AI tools were used as a supplementary learning and development aid during the project.

AI assistance was used for:

* Clarifying Computer Networks concepts and protocols relevant to the project.
* Understanding possible implementation approaches and discussing architectural alternatives and their trade-offs.
* Identifying debugging approaches for compiler, runtime, and networking errors.
* Developing test cases and identifying possible edge cases and failure scenarios.
* Improving the clarity and organization of project documentation and README material.
* Generating code for unfamiliar technical flows where the team does not have prior knowledge of the required implementation, based on clearly specified project requirements.

AI-assisted suggestions for unfamiliar technical flows, debugging, tests, and documentation were reviewed and verified before incorporation. Team members remain responsible for understanding the code and its design decisions.

The team is responsible for the design and implementation. AI-assisted suggestions do not replace review or the team's understanding of its contributions.

---

# 15. Development Approach

The project was developed incrementally, and the final implementation was validated only after the feature set had been stabilized.

Development proceeded through the following stages:

```text
Stage 1
Project setup
      │
      ▼
Stage 2
TCP connection handling
      │
      ▼
Stage 3
HTTP parsing & forwarding
      │
      ▼
Stage 4
Concurrent client handling
      │
      ▼
Stage 5
Caching
      │
      ▼
Stage 6
Access control
      │
      ▼
Stage 7
Logging & metrics
      │
      ▼
Stage 8
Error handling
      │
      ▼
Stage 9
Testing & benchmarking
      │
      ▼
Stage 10
Integration & final demonstration
```

---

# 16. Delivered Outcome

The implemented **HTTP forward proxy server** provides:

* Handling multiple simultaneous clients
* Parsing and forwarding HTTP traffic
* Caching eligible responses
* Distinguishing cache hits and misses
* Enforcing access-control rules
* Recording useful traffic information
* Handling network and protocol failures gracefully
* Measuring latency and throughput
* Demonstrating the effects of caching and proxy overhead

The submission includes source code, setup instructions, design and test documentation, performance evaluation, and a local demonstration workflow.

---

# 17. Project Development Sequence

| Phase        | Development area                                           |
| ------------ | --------------------------------------------------------- |
| **Phase 1**  | Project setup and architecture                             |
| **Phase 2**  | TCP connection handling and basic proxy                   |
| **Phase 3**  | HTTP request/response parsing and forwarding              |
| **Phase 4**  | Concurrent client handling                                |
| **Phase 5**  | Cache implementation and cache policy                     |
| **Phase 6**  | Access control and configuration                          |
| **Phase 7**  | Logging, metrics and error handling                       |
| **Phase 8**  | Integration and comprehensive testing                     |
| **Phase 9**  | Performance benchmarking and analysis                     |
| **Phase 10** | Documentation, final testing and demonstration            |

---

# 18. Project Deliverables

The final project provides:

* Working proxy server
* Complete source code
* Setup and execution instructions
* Design documentation
* Configuration for access-control rules
* Cache implementation
* Traffic logs and metrics
* Test cases and results
* Performance measurements
* Direct-vs-proxy comparison
* Demonstration of required scenarios

The project specification also requires a working proxy, design document, live demonstration, and performance measurements.

---

# 19. Conclusion

Through this project, the team built a practical networking system demonstrating how a proxy server acts as an intermediary between clients and destination servers while providing caching, access control, traffic monitoring, concurrency, and error handling.

The integrated system applies Computer Networks concepts and measures practical trade-offs involved in implementing a network intermediary.

---

## Project Information

**Project:** Project 1 — Designing a Proxy Server
**Course:** Computer Networks
**Team Size:** 4
**Final Submission:** 9 October 2026

---
