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

Each stage reports requests per second, successes, failures, p50/p95 latency,
and average/peak server RSS. Saturation can be signaled by errors, a throughput
drop, or a throughput plateau while latency rises. The ramp stops at the
selected maximum, or at the first detected saturation when
`--stop-on-saturation` is supplied.

The default maximum is 512 concurrent clients and can be raised to 4096. The
server PID is detected from the listening port with `lsof`; pass `--pid PID`
if detection is ambiguous. With Caddy, pass the C server PID to measure the C
process rather than the proxy. Options include:

| Option | Purpose |
| --- | --- |
| `--url` | Server origin URL |
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
and ephemeral ports. The configured `worker_threads` caps server workers at
64. Debug logging records every request and may become the bottleneck; use a
non-debug log level and disable console/file logging for a maximum-throughput
run.

RSS is whole-process memory, not a per-client allocation estimate. Results
depend on the host, network, database, and build configuration.
