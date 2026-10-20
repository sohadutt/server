#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: setup_server.sh [--tls] [config-file]

Builds the project and starts the server. Without --tls, serves HTTP using the
selected config file. With --tls, requires Caddy and serves HTTPS at
https://localhost:8443 while keeping the C server on 127.0.0.1:8080.
EOF
}

tls=0
config_file="config/server.conf"
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi
if [[ "${1:-}" == "--tls" ]]; then
  tls=1
  shift
fi
if [[ "$#" -gt 1 ]]; then
  usage >&2
  exit 2
fi
if [[ "$#" -eq 1 ]]; then
  config_file="$1"
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
cd "$project_dir"

if [[ "$config_file" != /* ]]; then
  config_file="$project_dir/$config_file"
fi
if [[ ! -f "$config_file" ]]; then
  printf 'Config file not found: %s\n' "$config_file" >&2
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  printf 'CMake 3.30 or newer is required.\n' >&2
  exit 1
fi
if (( tls )) && ! command -v caddy >/dev/null 2>&1; then
  printf 'TLS mode requires Caddy. Install Caddy, then run this script again.\n' >&2
  printf 'See https://caddyserver.com/docs/install for installation instructions.\n' >&2
  exit 1
fi

cmake -S "$project_dir" -B "$project_dir/build"
cmake --build "$project_dir/build" --parallel

if (( ! tls )); then
  exec "$project_dir/build/http_server" "$config_file"
fi

# The generated config may contain the print bearer token, so keep it private.
umask 077
tls_dir="$project_dir/build/local-tls"
mkdir -p "$tls_dir"
server_config="$tls_dir/server.conf"
caddyfile="$tls_dir/Caddyfile"
{
  cat "$config_file"
  printf '\n# Keep the backend private behind the local TLS proxy.\nhost = 127.0.0.1\nport = 8080\n'
} > "$server_config"
chmod 600 "$server_config"

cat > "$caddyfile" <<'EOF'
{
  admin off
}

https://localhost:8443 {
  tls internal
  reverse_proxy 127.0.0.1:8080
}
EOF

"$project_dir/build/http_server" "$server_config" &
server_pid=$!

cleanup() {
  result=$?
  trap - EXIT INT TERM
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  exit "$result"
}
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
  wait "$server_pid" || true
  printf 'The HTTP server did not start; check its configuration and logs.\n' >&2
  exit 1
fi

printf 'HTTPS is available at https://localhost:8443 (Ctrl-C stops both processes).\n'
printf 'Caddy uses its local CA; run `caddy trust` if your browser does not trust it.\n'
caddy run --config "$caddyfile" --adapter caddyfile
