#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"

usage() {
  cat <<'HELP'
Usage:
  scripts/setup.sh [run options]       Build and start local HTTPS (default)
  scripts/setup.sh check               Configure and build only
  scripts/setup.sh cert [options]      Create a development TLS certificate
  scripts/setup.sh cups ACTION         Manage CUPS and printer queues
  scripts/setup.sh server [options]    Build and run a configured server

Run options:
  --bind ADDRESS  Address to listen on (default: 127.0.0.1)
  --name HOST     DNS name or IP in the certificate (default: localhost)
  --port PORT     HTTPS port (default: 8443)
  --with-cups     Start/enable the system CUPS service before building
  --force-cert    Replace an existing development certificate and key
  --check         Configure and build only; do not create secrets or start
  -h, --help      Show this help

Certificate:
  setup.sh cert [--force] [hostname-or-ip] [valid-days]

CUPS actions:
  setup.sh cups start
  setup.sh cups status
  setup.sh cups discover
  setup.sh cups add-printer [queue-name] [printer-uri] [description]

Server:
  setup.sh server [config-file]
  setup.sh server --native-tls [config-file]
  setup.sh server --tls [config-file]  Use optional local Caddy proxy
HELP
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Required command not found: %s\n' "$1" >&2
    return 1
  fi
}

build_project() {
  require_command cmake || return
  cmake -S "$project_dir" -B "$project_dir/build"
  cmake --build "$project_dir/build" --parallel
}

make_certificate() {
  local force=0 hostname=localhost days=365 san cert_dir key_file cert_file
  if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    sed -n '/^Certificate:/,/^CUPS actions:/p' < <(usage) | sed '$d'
    return 0
  fi
  if [[ "${1:-}" == --force ]]; then force=1; shift; fi
  (($# <= 2)) || { printf 'Usage: setup.sh cert [--force] [hostname-or-ip] [valid-days]\n' >&2; return 2; }
  hostname="${1:-localhost}"
  days="${2:-365}"
  if [[ ! "$days" =~ ^[1-9][0-9]*$ ]] || ((${#days} > 3)) || ((10#$days > 825)); then
    printf 'Certificate validity must be between 1 and 825 days.\n' >&2; return 2
  fi
  require_command openssl || return
  if [[ "$hostname" == *:* || "$hostname" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    san="IP:$hostname"
  elif [[ "$hostname" =~ ^[A-Za-z0-9*.-]+$ ]]; then
    san="DNS:$hostname"
  else
    printf 'Use a DNS hostname or IP address for the certificate SAN.\n' >&2; return 2
  fi
  cert_dir="$project_dir/certs"; key_file="$cert_dir/server.key"; cert_file="$cert_dir/server.crt"
  mkdir -p "$cert_dir"
  if (( ! force )) && { [[ -e "$key_file" ]] || [[ -e "$cert_file" ]]; }; then
    printf 'Certificate or key already exists. Use --force to replace it.\n' >&2; return 1
  fi
  umask 077
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -noenc \
    -days "$days" -subj '/CN=Development Certificate' -addext "subjectAltName=$san" \
    -keyout "$key_file" -out "$cert_file"
  chmod 600 "$key_file"; chmod 644 "$cert_file"
  printf 'Created development certificate for %s (%s days).\n' "$hostname" "$days"
  printf 'Certificate: %s\nPrivate key: %s\n' "$cert_file" "$key_file"
  printf 'This self-signed certificate is for development; use a trusted CA certificate for public deployments.\n'
}

run_privileged() {
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then "$@"
  elif command -v sudo >/dev/null 2>&1; then sudo "$@"
  else printf 'This CUPS operation needs administrator privileges.\n' >&2; return 1
  fi
}

cups_status() {
  require_command lpstat || return
  lpstat -r
  printf '\nConfigured printers and classes:\n'
  lpstat -p -d 2>/dev/null || true
}

cups_start() {
  require_command lpstat || return
  case "$(uname -s)" in
    Linux)
      if command -v systemctl >/dev/null 2>&1; then
        if systemctl cat cups.service >/dev/null 2>&1; then run_privileged systemctl enable --now cups.service
        elif systemctl cat cups.socket >/dev/null 2>&1; then run_privileged systemctl enable --now cups.socket
        else printf 'Could not find cups.service or cups.socket in systemd.\n' >&2; return 1; fi
      elif command -v service >/dev/null 2>&1; then run_privileged service cups start
      else printf 'Could not find systemctl or service to start CUPS.\n' >&2; return 1; fi
      ;;
    Darwin)
      require_command launchctl || return
      local plist=/System/Library/LaunchDaemons/org.cups.cupsd.plist
      [[ -f "$plist" ]] || { printf 'CUPS launch daemon not found: %s\n' "$plist" >&2; return 1; }
      run_privileged launchctl enable system/org.cups.cupsd
      if ! launchctl print system/org.cups.cupsd >/dev/null 2>&1; then run_privileged launchctl bootstrap system "$plist"; fi
      run_privileged launchctl kickstart system/org.cups.cupsd
      ;;
    *) printf 'Automatic CUPS service setup is supported on Linux and macOS. Start it with your OS service manager.\n' >&2; return 1 ;;
  esac
  cups_status
}

cups_discover() {
  require_command lpinfo || return
  lpinfo -v | awk '$1 == "network" && $2 ~ /^(ipp|ipps|dnssd):\/\// { print $2 }'
}

cups_add_printer() {
  local queue_name="${1:-}" printer_uri="${2:-}" description="${3:-}" candidate
  (($# <= 3)) || { printf 'Usage: setup.sh cups add-printer [queue-name] [printer-uri] [description]\n' >&2; return 2; }
  local command_name
  for command_name in lpinfo lpadmin lpstat awk; do require_command "$command_name" || return; done
  if [[ -z "$printer_uri" ]]; then
    local -a candidates=()
    while IFS= read -r candidate; do [[ -n "$candidate" ]] && candidates+=("$candidate"); done \
      < <(lpinfo -v | awk '$1 == "network" { print $2 }' | awk '/^(ipp|ipps|dnssd):\/\//')
    if ((${#candidates[@]} == 0)); then
      printf 'No IPP destinations discovered. Enter an ipp:// or ipps:// URI: '
      IFS= read -r printer_uri
    else
      printf 'Discovered IPP destinations:\n'
      select candidate in "${candidates[@]}" 'Enter a URI manually'; do
        if [[ -n "${candidate:-}" && "$candidate" == 'Enter a URI manually' ]]; then
          printf 'Printer URI: '; IFS= read -r printer_uri; break
        elif [[ -n "${candidate:-}" ]]; then printer_uri="$candidate"; break
        else printf 'Choose a listed number.\n' >&2; fi
      done
    fi
  fi
  [[ "$printer_uri" =~ ^(ipp|ipps|dnssd)://[^[:space:]]+$ ]] || {
    printf 'Use a discovered ipp://, ipps://, or dnssd:// URI.\n' >&2; return 2; }
  if [[ -z "$queue_name" ]]; then printf 'CUPS queue name: '; IFS= read -r queue_name; fi
  [[ "$queue_name" =~ ^[A-Za-z0-9_.-]+$ ]] || {
    printf 'Queue names may contain only letters, digits, dot, underscore, and hyphen.\n' >&2; return 2; }
  if [[ -z "$description" ]]; then printf 'Description (optional): '; IFS= read -r description; fi
  if lpstat -p "$queue_name" >/dev/null 2>&1; then
    printf 'CUPS queue already exists: %s\n' "$queue_name" >&2; return 1
  fi
  local -a lpadmin_args=(-p "$queue_name" -E -v "$printer_uri" -m everywhere)
  [[ -z "$description" ]] || lpadmin_args+=(-D "$description")
  printf 'Adding queue %s for %s\n' "$queue_name" "$printer_uri"
  run_privileged lpadmin "${lpadmin_args[@]}"
  lpstat -p "$queue_name" -l
  printf 'Added CUPS queue: %s\n' "$queue_name"
}

cups_command() {
  local action="${1:-start}"; shift || true
  case "$action" in
    start) (($# == 0)) || return 2; cups_start ;;
    status) (($# == 0)) || return 2; cups_status ;;
    discover) (($# == 0)) || return 2; cups_discover ;;
    add-printer) cups_add_printer "$@" ;;
    -h|--help|help) printf 'Usage: setup.sh cups {start|status|discover|add-printer [queue-name] [printer-uri] [description]}\n' ;;
    *) printf 'Unknown CUPS action: %s\n' "$action" >&2; return 2 ;;
  esac
}

server_command() {
  local mode=normal config_file=config/server.conf
  case "${1:-}" in
    --help|-h) usage; return 0 ;;
    --tls) mode=proxy; shift ;;
    --native-tls) mode=native; shift ;;
  esac
  (($# <= 1)) || { printf 'Usage: setup.sh server [--native-tls|--tls] [config-file]\n' >&2; return 2; }
  [[ $# -eq 0 ]] || config_file="$1"
  [[ "$config_file" == /* ]] || config_file="$project_dir/$config_file"
  [[ -f "$config_file" ]] || { printf 'Config file not found: %s\n' "$config_file" >&2; return 1; }
  build_project || return
  if [[ "$mode" == normal ]]; then exec "$project_dir/build/http_server" "$config_file"; fi
  if [[ "$mode" == native ]]; then
    grep -Eq '^[[:space:]]*tls_enabled[[:space:]]*=[[:space:]]*(true|1)[[:space:]]*$' "$config_file" || {
      printf 'Native TLS requires tls_enabled = true (or 1) in %s.\n' "$config_file" >&2; return 2; }
    exec "$project_dir/build/http_server" "$config_file"
  fi
  require_command caddy || { printf 'Install Caddy to use the optional reverse proxy.\n' >&2; return 1; }
  if [[ ! -f "$project_dir/certs/server.crt" || ! -f "$project_dir/certs/server.key" ]]; then make_certificate localhost 365 || return; fi
  local tls_dir="$project_dir/build/local-tls" server_config="$project_dir/build/local-tls/server.conf" caddyfile="$project_dir/build/local-tls/Caddyfile"
  mkdir -p "$tls_dir"
  {
    cat "$config_file"
    printf '\n# Generated private TLS backend config.\nhost = 127.0.0.1\nport = 8080\ntls_enabled = true\ntls_certificate_file = %s/certs/server.crt\ntls_private_key_file = %s/certs/server.key\n' "$project_dir" "$project_dir"
  } > "$server_config"
  chmod 600 "$server_config"
  cat > "$caddyfile" <<'CADDY'
{
  admin off
}

https://localhost:8443 {
  tls internal
  reverse_proxy https://127.0.0.1:8080 {
    transport http {
      tls_insecure_skip_verify
    }
  }
}
CADDY
  "$project_dir/build/http_server" "$server_config" &
  local server_pid=$!
  cleanup_server() { local result=$?; trap - EXIT INT TERM; kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; exit "$result"; }
  trap cleanup_server EXIT INT TERM
  sleep 1
  kill -0 "$server_pid" 2>/dev/null || { printf 'The HTTP server did not start; check config and logs.\n' >&2; return 1; }
  printf 'HTTPS is available at https://localhost:8443 (Ctrl-C stops both processes).\n'
  printf 'Caddy uses its local CA; run `caddy trust` if needed.\n'
  caddy run --config "$caddyfile" --adapter caddyfile
}

run_local() {
  local bind_address=127.0.0.1 certificate_name=localhost port=8443 with_cups=0 force_cert=0 check_only=0
  while (($#)); do
    case "$1" in
      --bind) (($# >= 2)) || { usage >&2; return 2; }; bind_address=$2; shift 2 ;;
      --name) (($# >= 2)) || { usage >&2; return 2; }; certificate_name=$2; shift 2 ;;
      --port) (($# >= 2)) || { usage >&2; return 2; }; port=$2; shift 2 ;;
      --with-cups) with_cups=1; shift ;;
      --force-cert) force_cert=1; shift ;;
      --check) check_only=1; shift ;;
      -h|--help) usage; return 0 ;;
      *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; return 2 ;;
    esac
  done
  [[ "$port" =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || {
    printf 'Port must be an integer from 1 to 65535.\n' >&2; return 2; }
  [[ "$bind_address" =~ ^[A-Za-z0-9._:%-]+$ && -n "$certificate_name" && "$certificate_name" != *$'\n'* ]] || {
    printf 'Bind address and certificate name must be non-empty single-line values.\n' >&2; return 2; }
  if ((with_cups)); then cups_start || return; fi
  build_project || return
  ((check_only)) && { printf 'Build and dependency checks completed.\n'; return 0; }
  local certificate="$project_dir/certs/server.crt" private_key="$project_dir/certs/server.key"
  if ((force_cert)); then make_certificate --force "$certificate_name" 365 || return
  elif [[ ! -e "$certificate" && ! -e "$private_key" ]]; then make_certificate "$certificate_name" 365 || return
  elif [[ ! -f "$certificate" || ! -f "$private_key" ]]; then
    printf 'Only part of the TLS certificate pair exists. Repair it with --force-cert.\n' >&2; return 1
  fi
  local runtime_dir="$project_dir/build/local-setup" admin_token_file runtime_config admin_token
  mkdir -p "$runtime_dir"; chmod 700 "$runtime_dir"
  admin_token_file="$runtime_dir/admin.token"
  if [[ ! -s "$admin_token_file" ]]; then openssl rand -hex 32 > "$admin_token_file"; fi
  chmod 600 "$admin_token_file"
  admin_token="$(<"$admin_token_file")"
  [[ "$admin_token" =~ ^[A-Fa-f0-9]{64}$ ]] || { printf 'Admin-token file is invalid: %s\n' "$admin_token_file" >&2; return 1; }
  runtime_config="$runtime_dir/server.conf"
  {
    cat "$project_dir/config/server.conf"
    cat <<CONFIG

# Generated by scripts/setup.sh. Keep this file private.
host = $bind_address
port = $port
tls_enabled = true
tls_certificate_file = $certificate
tls_private_key_file = $private_key
admin_api_token = $admin_token
CONFIG
  } > "$runtime_config"
  chmod 600 "$runtime_config"
  printf 'Starting HTTPS at https://%s:%s/\n' "$certificate_name" "$port"
  printf 'Admin credential is stored in %s (mode 0600).\n' "$admin_token_file"
  printf 'This development certificate must be trusted by your browser/device.\n'
  exec "$project_dir/build/http_server" "$runtime_config"
}

cd "$project_dir"
case "${1:-}" in
  -h|--help) usage ;;
  cert) shift; make_certificate "$@" ;;
  cups) shift; cups_command "$@" ;;
  server) shift; server_command "$@" ;;
  check) shift; (($# == 0)) || { usage >&2; exit 2; }; build_project ;;
  *) run_local "$@" ;;
esac
