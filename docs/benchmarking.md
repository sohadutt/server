# Load and memory benchmarking

The benchmark ramps load in stages and sustains each concurrency level before
moving to the next. Static requests use HTTP/1.1 keep-alive by default, so each
client reuses its TCP/TLS connection for up to 16 requests. Use
`--connection-mode close` to measure a new connection and TLS handshake for
every request. API routes currently close after each response.

## Static site load

Start the server, then run the benchmark from the project root:

```sh
./scripts/benchmark.sh --path / --max-concurrency 512 --step 32 --stage-duration 10
```

This measures the request path without SQLite session lookup.

## Authenticated API load

To include authenticated session lookup and SQLite work, use an existing
active test account:

```sh
./scripts/benchmark.sh --path /api/me --phone +14155550123 \
  --max-concurrency 512 --step 32 --stage-duration 10
```

The API workload inserts temporary one-hour sessions into the configured
SQLite database and revokes only those sessions when it exits. If forcibly
stopped, the sessions expire automatically. Use a test account and database
for API load runs.

## Results and options

Each stage reports successful requests per second separately from attempted
requests per second, successes, failures, p50/p95 latency, and average/peak
server RSS. Failures include a reason such as an HTTP status, TLS error,
timeout, or socket error. This helps distinguish server overload from client
limits such as ephemeral-port exhaustion. Saturation can be signaled by errors,
a throughput drop, or a throughput plateau while latency rises. The ramp stops
at the selected maximum, or at the first detected saturation when
`--stop-on-saturation` is supplied.

The default maximum is 512 concurrent clients and can be raised to 4096. The
server PID is detected from the listening port with `lsof`; pass `--pid PID`
if detection is ambiguous. With Caddy, pass the C server PID to measure the C
process rather than the proxy. Options include:

| Option | Purpose |
| --- | --- |
| `--url` | Server origin URL (defaults to `https://localhost:8443`) |
| `--ca-cert` | CA/certificate file for HTTPS verification; defaults to `certs/server.crt` |
| `--path` | Request path; `/` for static load or `/api/me` for authenticated load |
| `--phone` | Existing active test user for `/api/me` |
| `--database` | SQLite path; defaults to `database_path` in `server.conf` |
| `--pid` | Process ID to use for memory measurements |
| `--max-concurrency` | Maximum concurrent clients, 1–4096 |
| `--step` | Concurrency increase between stages |
| `--stage-duration` | Seconds to sustain each stage |
| `--timeout` | Per-request timeout |
| `--connection-mode` | `keep-alive` (default) or `close` after each request |
| `--stop-on-saturation` | End the ramp at the first saturation signal |

For a server ceiling measurement, run the load generator from another machine
on the same network so it does not compete for CPU and memory. Close mode opens
a fresh connection per request and can exhaust the client's ephemeral ports
during localhost runs. Compare modes separately because they measure different
work: keep-alive emphasizes request handling, while close mode includes TCP and
TLS setup costs.
The configured `worker_threads` caps server workers at 64. Static keep-alive
sockets release their worker between requests and are watched by the event
loop. TLS handshakes and active request reads/writes still use workers. The
server uses a bounded queue of up to 64 waiting connections per worker; queue
rejections and current worker/connection counts are logged every five seconds.
Debug logging records each request and may become the bottleneck; use a
non-debug log level and disable console/file logging for a maximum-throughput
run.

Before load starts, the benchmark sends a single preflight request and requires
HTTP 200. This catches wrong paths (including a missing configured 404 page)
before they contaminate the throughput stages. For meaningful CPU comparisons,
repeat each mode several times and compare medians; localhost runs share CPU
between the server and Python client.

Each stage creates exactly the requested number of sequential client worker
tasks. In keep-alive mode, a worker keeps one HTTP connection and sends one
request at a time; it reconnects if the server closes that connection. In
close mode, the worker opens and closes a connection for every request. Stage
output includes the peak number of simultaneous request coroutines and fully
established connections. Established connections can stay below the worker
count while TCP connects or TLS handshakes are in progress; the benchmark does
not count in-progress handshakes as established sockets.

Latency starts when a worker begins a request and ends after it reads the full
response body. It includes connection and TLS handshake time whenever a new
connection is needed, plus client event-loop and protocol time during that
request. It excludes idle time between requests and scheduling time before a
worker starts its next request. The server logs five-second samples for process
CPU, average worker-queue delay, worker service time, TLS handshake,
request-read, response-write, and explicit writable-wait durations. CPU above
100% means multiple cores are active. Compare these with 32–128 client runs:
high CPU and TLS/write time point toward CPU or crypto work; low CPU with high
queue, read, or writable wait points toward worker saturation or I/O waiting.
Use `--max-concurrency 128 --step 32` for this range, then compare
`--connection-mode keep-alive` with `--connection-mode close`.

## Local static-file baseline

One post-change loopback sample on macOS used the same 377-byte
`/index.html`, 32 server workers, and five-second stages. All requests
succeeded. Treat these as a local regression baseline, not a capacity promise:

| Protocol and mode | 32 clients | 64 clients |
| --- | --- | --- |
| HTTP keep-alive | 25,802 req/s, p95 1.8 ms | 25,530 req/s, p95 3.6 ms |
| HTTP close | 12,941 req/s, p95 2.6 ms | 13,513 req/s, p95 5.0 ms |
| HTTPS keep-alive | 13,976 req/s, p95 7.6 ms | 13,519 req/s, p95 15.5 ms |
| HTTPS close | 2,475 req/s, p95 13.0 ms | 2,731 req/s, p95 19.9 ms |

RSS is whole-process memory, not a per-client allocation estimate. Results
depend on the host, network, database, and build configuration.
