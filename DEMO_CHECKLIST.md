# Proxy Server Demo Checklist

This demonstration uses the bundled loopback origin server. It does not need
Internet access and uses two terminals for the services plus a client terminal.
This project implements a bounded educational HTTP forward proxy supporting
HTTP/1.0 and HTTP/1.1 GET traffic. HTTPS CONNECT tunneling, IPv6 authority
literals, request bodies, and non-GET methods are outside the implemented
scope. This is not a general-purpose browser proxy.

## 1. Build and start the local origin

From the repository root:

```sh
make
make build/local_origin_server
mkdir -p /tmp/cn-proxy-demo
cd /tmp/cn-proxy-demo
/home/kush/CN/build/local_origin_server 18080
```

Keep this terminal running. The fixture binds to loopback and serves a
deterministic `/hello` response.

## 2. Start the proxy

In a second terminal:

```sh
cd /tmp/cn-proxy-demo
/home/kush/CN/build/proxy --port 18081 --cache-ttl 3 \
  --access-control /home/kush/CN/config/access_control.conf
```

Keep this terminal running. The sample policy explicitly blocks
`blocked.test`; all other destinations are allowed by default.

## 3. Forward a request and demonstrate cache MISS/HIT

In a third terminal:

```sh
cd /tmp/cn-proxy-demo
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  "http://127.0.0.1:18080/cache?demo=1"
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  "http://127.0.0.1:18080/cache?demo=1"
```

Both calls should return HTTP 200 and the same body. Inspect the corresponding
records to see the first MISS and subsequent HIT:

```sh
grep 'Host=127.0.0.1' proxy.log
```

Demonstrate normal forwarding with a separate uncached URL:

```sh
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  "http://127.0.0.1:18080/hello"
```

## 4. Demonstrate cache expiry

The proxy is configured with a three-second TTL:

```sh
sleep 4
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  "http://127.0.0.1:18080/cache?demo=1"
grep 'Host=127.0.0.1' proxy.log
```

The post-expiry request should be logged as a MISS and fetched again.

## 5. Demonstrate access-control BLOCK

```sh
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  "http://blocked.test:18080/never"
```

Expect HTTP 403. The rule is checked before origin DNS/connect, so the blocked
test hostname need not resolve. Confirm the 403 record:

```sh
grep 'Host=blocked.test' proxy.log
```

## 6. Close services

Send Ctrl-C in the proxy and origin terminals. The proxy prints its aggregate
metrics and writes `metrics.csv` in `/tmp/cn-proxy-demo`.

## Automated counterpart

The full automated run is:

```sh
cd /home/kush/CN
make clean
make
make test
```

It checks forwarding, cache MISS/HIT/expiry, BLOCK, malformed input,
unreachable and timed-out origins, concurrent requests, the 64-worker limit,
recovery, logging, metrics, and direct-versus-proxy localhost benchmarks.
