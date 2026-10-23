# Load and memory benchmarking

The benchmark ramps load in stages, opens a new TCP connection for every HTTP
request, and sustains each concurrency level before moving to the next. It
exercises connection acceptance, the worker queue, request handling, and
response delivery.

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
| `--stop-on-saturation` | End the ramp at the first saturation signal |

For a server ceiling measurement, run the load generator from another machine
on the same network so it does not compete for CPU, memory, file descriptors,
and ephemeral ports. This benchmark opens a fresh connection per request, so a
localhost run can exhaust the client's ephemeral ports and distort results.
The configured `worker_threads` caps server workers at 64. The server uses a
bounded queue of up to 64 waiting connections per worker; very high concurrency
may still be limited by the process file-descriptor limit. Debug logging records
each request and may become the bottleneck; use a non-debug log level and
disable console/file logging for a maximum-throughput run.

RSS is whole-process memory, not a per-client allocation estimate. Results
depend on the host, network, database, and build configuration.


The table below is a historical run from before the listener-drain, worker
queue, and benchmark-accounting fixes. Its throughput column counted attempts,
so it must not be interpreted as successful requests per second. Rerun the
benchmark with the current version to obtain comparable success rates and
failure reasons.

```text
./scripts/benchmark.sh --path / --max-concurrency 4000 --step 32 --stage-duration 10
Target: https://localhost:8443/
Server PID: 70020; baseline RSS: 10.0 MiB
Load: 32 to 4000 concurrent clients, 10s per stage

Concurrency | Throughput | Successful | Failed | Latency | Server RSS
   32 clients |    2569.6 req/s |    25696 ok |      0 failed | p50    10.4 ms | p95    11.4 ms | RSS avg/peak 11.6/11.7 MiB
   64 clients |    3854.0 req/s |    13614 ok |  24926 failed | p50     8.5 ms | p95    32.3 ms | RSS avg/peak 11.8/11.8 MiB
   96 clients |    4460.5 req/s |     8580 ok |  36025 failed | p50    14.3 ms | p95    49.5 ms | RSS avg/peak 11.9/12.0 MiB
  128 clients |    4721.3 req/s |     6097 ok |  41116 failed | p50    19.9 ms | p95    67.9 ms | RSS avg/peak 12.1/12.2 MiB
  160 clients |    4805.2 req/s |     4825 ok |  43227 failed | p50    26.0 ms | p95    84.7 ms | RSS avg/peak 12.2/12.2 MiB
  192 clients |    4850.1 req/s |     4040 ok |  44461 failed | p50    32.3 ms | p95    99.9 ms | RSS avg/peak 12.2/12.3 MiB
  224 clients |    4908.7 req/s |     3438 ok |  45649 failed | p50    38.3 ms | p95   115.0 ms | RSS avg/peak 12.3/12.3 MiB
  256 clients |    4864.8 req/s |     2954 ok |  45694 failed | p50    45.1 ms | p95   131.0 ms | RSS avg/peak 12.3/12.3 MiB
  288 clients |    4618.7 req/s |     2541 ok |  43646 failed | p50    54.0 ms | p95   147.7 ms | RSS avg/peak 12.5/12.6 MiB
```
