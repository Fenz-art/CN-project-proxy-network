# Live Proxy Demo

This guide runs the project against its deterministic local HTTP origin. It
demonstrates normal forwarding, cache MISS/HIT, and the configured hostname
block rule without relying on an external website.

This project implements a bounded educational HTTP forward proxy supporting
HTTP/1.0 and HTTP/1.1 GET traffic. HTTPS CONNECT tunneling, IPv6 authority
literals, request bodies, and non-GET methods are outside the implemented
scope.

## Build

From the repository root:

```sh
make clean
make
make build/local_origin_server
mkdir -p /tmp/cn-proxy-demo
```

## Terminal 1: start the origin

```sh
cd /tmp/cn-proxy-demo
/home/kush/CN/build/local_origin_server 18080
```

The fixture listens on loopback port `18080`. Keep this terminal open.

## Terminal 2: start the proxy

```sh
cd /tmp/cn-proxy-demo
rm -f proxy.log metrics.csv
/home/kush/CN/build/proxy --port 18081 --cache-ttl 3 \
  --access-control /home/kush/CN/config/access_control.conf
```

The proxy listens on loopback port `18081`. The sample access policy blocks
`blocked.test`. Keep this terminal open.

## Terminal 3: forwarding and cache demonstration

Send the same HTTP GET twice:

```sh
cd /tmp/cn-proxy-demo
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  http://127.0.0.1:18080/hello
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  http://127.0.0.1:18080/hello
```

Both responses should be `HTTP/1.1 200 OK` with body:

```text
hello from local origin
```

Show the proxy's cache decisions and the origin's request count:

```sh
grep 'URL=/hello' proxy.log
curl --noproxy '*' -sS \
  'http://127.0.0.1:18080/_count?path=/hello'
```

The proxy log should show the first request as `Status=200 | Cache=MISS` and
the second as `Status=200 | Cache=HIT`. The origin count should be `1`,
confirming the hit did not make another origin request.

## Terminal 3: access-control demonstration

Check the origin count, request the blocked host through the proxy, then check
the count again:

```sh
curl --noproxy '*' -sS \
  'http://127.0.0.1:18080/_count?path=/never'
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  http://blocked.test:18080/never
curl --noproxy '*' -sS \
  'http://127.0.0.1:18080/_count?path=/never'
grep 'Host=blocked.test' proxy.log
```

The two origin counts should both be `0`. The blocked request should return
`HTTP/1.1 403 Forbidden`, and the proxy log should contain
`Host=blocked.test | URL=/never | Status=403`.

Confirm the proxy continues serving after the block:

```sh
curl --noproxy "" --proxy http://127.0.0.1:18081 -i \
  http://127.0.0.1:18080/hello
```

## Stop the demo

Press Ctrl-C in Terminal 2, then Terminal 1. The proxy prints aggregate
metrics and writes `metrics.csv` in `/tmp/cn-proxy-demo`.

## Full automated validation

From the repository root:

```sh
make clean
make
make test
```
