#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"

usage() {
  cat <<'HELP'
Usage:
  scripts/manage.sh                     Open the interactive setup menu
  scripts/manage.sh [run options]       Build, configure, and start local HTTPS
  scripts/manage.sh menu                Open the interactive setup menu
  scripts/manage.sh check               Configure and build only
  scripts/manage.sh config init [path] [--profile local|public] [--force]
                                         Generate a private config in config/
  scripts/manage.sh cert [options]      Create a development TLS certificate
  scripts/manage.sh cups ACTION         Manage CUPS and printer queues
  scripts/manage.sh server [options]    Build and run a configured server
  scripts/manage.sh static ACTION       Build and publish Vite/static apps
  scripts/manage.sh service-unit [config] Generate a systemd user-service unit

Run options:
  --bind ADDRESS  Address to listen on (default: 127.0.0.1)
  --name HOST     DNS name or IP in the certificate (default: localhost)
  --port PORT     HTTPS port (default: 8443)
  --config FILE   Base config to use when generating the runtime config
  --with-cups     Start/enable the system CUPS service before building
  --force-cert    Replace an existing development certificate and key
  --check         Configure and build only; do not create secrets or start
  --start         Start an existing build; generate config/server.conf if needed
  --build         Build the project only; do not create secrets or start
  -h, --help      Show this help

Certificate:
  manage.sh cert [--force] [hostname-or-ip] [valid-days]

CUPS actions:
  manage.sh cups start
  manage.sh cups status
  manage.sh cups discover
  manage.sh cups add-printer [queue-name] [printer-uri] [description]

Server:
  manage.sh server [config-file]        Start behind local Caddy (HTTP/2)
  manage.sh server --native-tls [config-file]
  manage.sh server --tls [config-file]  Alias for the default Caddy proxy mode
  The guided local HTTPS setup uses Caddy for HTTP/2; Caddy can also serve HTTP/3

Static Vite applications:
  manage.sh static build NAME SOURCE_DIR [https://your-domain]
  manage.sh static add NAME DIST_DIR [https://your-domain]
  manage.sh static list
  manage.sh static remove NAME

`build` runs `npm ci` and `npm run build -- --base=/NAME/`, then installs the
Vite `dist/` output. `add` installs an already-built directory. Both publish
the app at `/NAME/` and enable SPA route fallback.
HELP
}

raise_open_file_limit() {
  local requested_limit="${SERVER_NOFILE_LIMIT:-65536}"
  local hard_limit target_limit effective_limit

  if [[ ! "$requested_limit" =~ ^[1-9][0-9]*$ ]]; then
    printf 'SERVER_NOFILE_LIMIT must be a positive integer (got %q).\n' "$requested_limit" >&2
    return 2
  fi

  hard_limit="$(ulimit -Hn)"
  target_limit="$requested_limit"
  if [[ "$hard_limit" != unlimited && "$hard_limit" =~ ^[0-9]+$ ]] &&
     (( target_limit > hard_limit )); then
    target_limit="$hard_limit"
  fi

  if ! ulimit -Sn "$target_limit" 2>/dev/null; then
    printf 'Could not raise the open-file limit to %s; continuing with the current limit.\n' "$target_limit" >&2
  fi

  effective_limit="$(ulimit -Sn)"
  printf 'Open-file limit: %s (requested %s; hard limit %s)\n' \
    "$effective_limit" "$requested_limit" "$hard_limit"
  if [[ "$effective_limit" =~ ^[0-9]+$ ]] &&
     (( effective_limit < requested_limit )); then
    printf 'Raise the OS or service-manager limit to allow more concurrent connections.\n' >&2
  fi
}

config_command() {
  local action="${1:-init}"
  if (($#)); then shift; fi
  case "$action" in
    init)
      local target="$project_dir/config/server.conf" force=0 path_set=0 profile=""
      while (($#)); do
        case "$1" in
          --force) force=1; shift ;;
          --profile)
            (($# >= 2)) || { printf 'Missing value for --profile.\n' >&2; return 2; }
            profile="$2"; shift 2 ;;
          -h|--help)
            printf 'Usage: manage.sh config init [path] [--profile local|public] [--force]\n'
            return 0
            ;;
          -* ) printf 'Unknown config option: %s\n' "$1" >&2; return 2 ;;
          *)
            ((path_set == 0)) || {
              printf 'Only one output path may be provided.\n' >&2; return 2;
            }
            target="$1"
            [[ "$target" == /* ]] || target="$project_dir/$target"
            path_set=1
            shift
            ;;
        esac
      done
      if [[ -z "$profile" ]]; then
        if [[ -t 0 ]]; then
          printf 'Choose a config profile:\n  1) Local development (loopback, HTTP)\n  2) Public server (all interfaces, HTTPS)\n'
          local profile_choice
          read -r -p 'Profile [1]: ' profile_choice || return 1
          case "${profile_choice:-1}" in
            1) profile=local ;;
            2) profile=public ;;
            *) printf 'Choose 1 (local) or 2 (public).\n' >&2; return 2 ;;
          esac
        else
          profile=local
          printf 'No interactive input; using the local config profile.\n'
        fi
      fi
      [[ "$profile" == local || "$profile" == public ]] || {
        printf 'Config profile must be local or public.\n' >&2; return 2; }
      local template="$project_dir/config/server.conf.example" target_dir target_name temp_file
      [[ -f "$template" ]] || { printf 'Config template not found: %s\n' "$template" >&2; return 1; }
      target_dir="$(dirname -- "$target")"
      target_name="$(basename -- "$target")"
      mkdir -p -- "$target_dir"
      if [[ -s "$target" && "$force" -ne 1 ]]; then
        printf 'Config already exists: %s (pass --force to replace it).\n' "$target" >&2
        return 1
      fi
      temp_file="$(mktemp "$target_dir/.${target_name}.XXXXXX")" || return
      local profile_host=127.0.0.1 profile_port=8080 profile_tls=false
      local profile_cert=certs/server.crt profile_key=certs/server.key profile_rate=true
      if [[ "$profile" == public ]]; then
        profile_host=0.0.0.0
        profile_port=8443
        profile_tls=true
      fi
      if ! sed \
          -e "s|@PROFILE@|$profile|g" \
          -e "s|@HOST@|$profile_host|g" \
          -e "s|@PORT@|$profile_port|g" \
          -e "s|@TLS_ENABLED@|$profile_tls|g" \
          -e "s|@TLS_CERTIFICATE@|$profile_cert|g" \
          -e "s|@TLS_PRIVATE_KEY@|$profile_key|g" \
          -e "s|@RATE_LIMITING_ENABLED@|$profile_rate|g" \
          "$template" > "$temp_file" || ! chmod 600 "$temp_file" ||
         ! mv -f -- "$temp_file" "$target"; then
        rm -f -- "$temp_file"
        printf 'Could not write config file: %s\n' "$target" >&2
        return 1
      fi
      printf 'Generated %s config: %s (mode 0600)\n' "$profile" "$target"
      if [[ "$profile" == public ]]; then
        printf 'Install a trusted certificate and matching private key at the configured paths before starting HTTPS.\n'
      else
        printf 'The local profile listens on loopback over HTTP. Use manage.sh for guided local HTTPS setup.\n'
      fi
      printf 'Keep TLS keys and admin tokens private.\n'
      ;;
    -h|--help|help)
      printf 'Usage: manage.sh config init [path] [--profile local|public] [--force]\n'
      ;;
    *) printf 'Unknown config action: %s\n' "$action" >&2; return 2 ;;
  esac
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Required command not found: %s\n' "$1" >&2
    return 1
  fi
}

package_is_installed() {
  local manager="$1" package="$2"
  case "$manager" in
    brew) brew list --versions "$package" >/dev/null 2>&1 ;;
    apt) [[ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)" == 'install ok installed' ]] ;;
    rpm|dnf|zypper) rpm -q "$package" >/dev/null 2>&1 ;;
    pacman) pacman -Q "$package" >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}

install_missing_packages() {
  local manager="$1"; shift
  local package
  local -a missing=()
  for package in "$@"; do
    package_is_installed "$manager" "$package" || missing+=("$package")
  done
  ((${#missing[@]} == 0)) && return 0

  printf 'Installing missing system dependencies: %s\n' "${missing[*]}"
  case "$manager" in
    brew) brew install "${missing[@]}" ;;
    apt)
      run_privileged apt-get update || return
      run_privileged apt-get install --yes "${missing[@]}"
      ;;
    dnf) run_privileged dnf install --assumeyes "${missing[@]}" ;;
    pacman) run_privileged pacman -Sy --needed --noconfirm "${missing[@]}" ;;
    zypper) run_privileged zypper --non-interactive install "${missing[@]}" ;;
    *) printf 'Unsupported package manager: %s\n' "$manager" >&2; return 1 ;;
  esac
}

ensure_build_dependencies() {
  case "$(uname -s)" in
    Darwin)
      if ! command -v brew >/dev/null 2>&1; then
        printf 'Homebrew is required to install missing macOS build packages. Install it from https://brew.sh, then rerun this command.\n' >&2
        return 1
      fi
      if ! xcrun --find clang >/dev/null 2>&1; then
        printf 'The Xcode Command Line Tools are required. Run: xcode-select --install\n' >&2
        return 1
      fi
      install_missing_packages brew cmake pkgconf openssl@3 python
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        install_missing_packages apt build-essential cmake libcups2-dev \
          libsqlite3-dev libssl-dev zlib1g-dev pkg-config python3 openssl
      elif command -v dnf >/dev/null 2>&1; then
        install_missing_packages dnf gcc cmake cups-devel sqlite-devel \
          openssl-devel zlib-devel pkgconf-pkg-config python3 openssl
      elif command -v pacman >/dev/null 2>&1; then
        install_missing_packages pacman base-devel cmake cups sqlite openssl \
          zlib pkgconf python
      elif command -v zypper >/dev/null 2>&1; then
        install_missing_packages zypper gcc cmake cups-devel sqlite3-devel \
          libopenssl-devel zlib-devel pkg-config python3 openssl
      else
        printf 'No supported package manager found (apt, dnf, pacman, or zypper). Install the CMake, C compiler, CUPS, SQLite, OpenSSL 3, zlib, pkg-config, Python 3, and OpenSSL CLI packages manually.\n' >&2
        return 1
      fi
      ;;
    *)
      printf 'Automatic dependency installation is supported on macOS and common Linux distributions.\n' >&2
      return 1
      ;;
  esac
}

ensure_node_dependencies() {
  if command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1; then
    return 0
  fi
  case "$(uname -s)" in
    Darwin)
      command -v brew >/dev/null 2>&1 || {
        printf 'Install Homebrew from https://brew.sh to install Node.js.\n' >&2
        return 1
      }
      install_missing_packages brew node
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        install_missing_packages apt nodejs npm
      elif command -v dnf >/dev/null 2>&1; then
        install_missing_packages dnf nodejs npm
      elif command -v pacman >/dev/null 2>&1; then
        install_missing_packages pacman nodejs npm
      elif command -v zypper >/dev/null 2>&1; then
        install_missing_packages zypper nodejs npm
      else
        printf 'Install Node.js and npm with your Linux distribution package manager.\n' >&2
        return 1
      fi
      ;;
    *) printf 'Install Node.js and npm before building a Vite app.\n' >&2; return 1 ;;
  esac
  require_command node && require_command npm
}

ensure_cups_dependencies() {
  if command -v lpstat >/dev/null 2>&1 && command -v lpinfo >/dev/null 2>&1 &&
     command -v lpadmin >/dev/null 2>&1; then
    return 0
  fi
  case "$(uname -s)" in
    Darwin)
      printf 'The CUPS command-line tools are not available. Enable/install the macOS CUPS components, then rerun this command.\n' >&2
      return 1
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        install_missing_packages apt cups cups-client
      elif command -v dnf >/dev/null 2>&1; then
        install_missing_packages dnf cups cups-client
      elif command -v pacman >/dev/null 2>&1; then
        install_missing_packages pacman cups
      elif command -v zypper >/dev/null 2>&1; then
        install_missing_packages zypper cups cups-client
      else
        printf 'Install CUPS and its client tools with your Linux package manager.\n' >&2
        return 1
      fi
      require_command lpstat && require_command lpinfo && require_command lpadmin
      ;;
    *)
      printf 'Automatic CUPS installation is supported on common Linux distributions. Install CUPS manually on this OS.\n' >&2
      return 1
      ;;
  esac
}

ensure_caddy_dependency() {
  command -v caddy >/dev/null 2>&1 && return 0
  case "$(uname -s)" in
    Darwin)
      command -v brew >/dev/null 2>&1 || {
        printf 'Install Homebrew from https://brew.sh to install Caddy.\n' >&2
        return 1
      }
      install_missing_packages brew caddy
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        install_missing_packages apt caddy
      elif command -v dnf >/dev/null 2>&1; then
        install_missing_packages dnf caddy
      elif command -v pacman >/dev/null 2>&1; then
        install_missing_packages pacman caddy
      elif command -v zypper >/dev/null 2>&1; then
        install_missing_packages zypper caddy
      else
        printf 'Install Caddy using its official instructions: https://caddyserver.com/docs/install\n' >&2
        return 1
      fi
      ;;
    *)
      printf 'Install Caddy using its official instructions: https://caddyserver.com/docs/install\n' >&2
      return 1
      ;;
  esac
  require_command caddy
}

build_project() {
  ensure_build_dependencies || return
  cmake -S "$project_dir" -B "$project_dir/build" || return
  cmake --build "$project_dir/build" --parallel || return
}

make_certificate() {
  local force=0 hostname=localhost days=365 san cert_dir key_file cert_file temp_dir
  if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    sed -n '/^Certificate:/,/^CUPS actions:/p' < <(usage) | sed '$d'
    return 0
  fi
  if [[ "${1:-}" == --force ]]; then force=1; shift; fi
  (($# <= 2)) || { printf 'Usage: manage.sh cert [--force] [hostname-or-ip] [valid-days]\n' >&2; return 2; }
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
  chmod 700 "$cert_dir"
  if (( ! force )) && { [[ -e "$key_file" ]] || [[ -e "$cert_file" ]]; }; then
    printf 'Certificate or key already exists. Use --force to replace it.\n' >&2; return 1
  fi
  umask 077
  temp_dir="$(mktemp -d "$cert_dir/.certificate.XXXXXX")" || return
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -noenc \
    -days "$days" -subj '/CN=Development Certificate' -addext "subjectAltName=$san" \
    -keyout "$temp_dir/server.key" -out "$temp_dir/server.crt" || {
      rm -rf -- "$temp_dir"
      return 1
    }
  chmod 600 "$temp_dir/server.key" && chmod 644 "$temp_dir/server.crt" || {
    rm -rf -- "$temp_dir"
    return 1
  }
  mv -f -- "$temp_dir/server.key" "$key_file" &&
    mv -f -- "$temp_dir/server.crt" "$cert_file" || {
      rm -rf -- "$temp_dir"
      return 1
    }
  rmdir "$temp_dir"
  printf 'Created development certificate for %s (%s days).\n' "$hostname" "$days"
  printf 'Certificate: %s\nPrivate key: %s\n' "$cert_file" "$key_file"
  printf 'This self-signed certificate is for development; use a trusted CA certificate for public deployments.\n'
}

run_privileged() {
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then "$@"
  elif command -v sudo >/dev/null 2>&1; then sudo "$@"
  else printf 'Installing system packages needs administrator privileges.\n' >&2; return 1
  fi
}

cups_status() {
  ensure_cups_dependencies || return
  lpstat -r
  printf '\nConfigured printers and classes:\n'
  lpstat -p -d 2>/dev/null || true
}

cups_start() {
  ensure_cups_dependencies || return
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
  ensure_cups_dependencies || return
  lpinfo -v | awk '$1 == "network" && $2 ~ /^(ipp|ipps|dnssd):\/\// { print $2 }'
}

cups_add_printer() {
  local queue_name="${1:-}" printer_uri="${2:-}" description="${3:-}" candidate
  (($# <= 3)) || { printf 'Usage: manage.sh cups add-printer [queue-name] [printer-uri] [description]\n' >&2; return 2; }
  ensure_cups_dependencies || return
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
    -h|--help|help) printf 'Usage: manage.sh cups {start|status|discover|add-printer [queue-name] [printer-uri] [description]}\n' ;;
    *) printf 'Unknown CUPS action: %s\n' "$action" >&2; return 2 ;;
  esac
}

server_command() {
  local mode=proxy config_file=config/server.conf
  case "${1:-}" in
    --help|-h) usage; return 0 ;;
    --tls) mode=proxy; shift ;;
    --native-tls) mode=native; shift ;;
  esac
  (($# <= 1)) || { printf 'Usage: manage.sh server [--native-tls|--tls] [config-file]\n' >&2; return 2; }
  [[ $# -eq 0 ]] || config_file="$1"
  [[ "$config_file" == /* ]] || config_file="$project_dir/$config_file"
  if [[ ! -s "$config_file" ]]; then
    local -a config_args=(init "$config_file")
    [[ ! -e "$config_file" ]] || config_args+=(--force)
    config_command "${config_args[@]}" || return
  fi
  build_project || return
  if [[ "$mode" == native ]]; then
    grep -Eq '^[[:space:]]*tls_enabled[[:space:]]*=[[:space:]]*(true|1)[[:space:]]*$' "$config_file" || {
      printf 'Native TLS requires tls_enabled = true (or 1) in %s.\n' "$config_file" >&2; return 2; }
    raise_open_file_limit
    exec "$project_dir/build/http_server" "$config_file"
  fi
  ensure_caddy_dependency || return
  if [[ ! -f "$project_dir/certs/server.crt" || ! -f "$project_dir/certs/server.key" ]]; then make_certificate localhost 365 || return; fi
  local tls_dir="$project_dir/build/local-tls" server_config="$project_dir/build/local-tls/server.conf" caddyfile="$project_dir/build/local-tls/Caddyfile"
  mkdir -p "$tls_dir"
  {
    cat "$config_file"
    printf '\n# Generated private TLS backend config.\nhost = 127.0.0.1\nport = 8444\ntls_enabled = true\ntls_certificate_file = %s/certs/server.crt\ntls_private_key_file = %s/certs/server.key\n' "$project_dir" "$project_dir"
  } > "$server_config"
  chmod 600 "$server_config"
  cat > "$caddyfile" <<'CADDY'
{
  admin off
}

https://localhost:8443 {
  tls internal
  reverse_proxy https://127.0.0.1:8444 {
    transport http {
      tls_insecure_skip_verify
    }
  }
}
CADDY
  raise_open_file_limit
  "$project_dir/build/http_server" "$server_config" &
  local server_pid=$!
  cleanup_server() { local result=$?; trap - EXIT INT TERM; kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; exit "$result"; }
  trap cleanup_server EXIT INT TERM
  sleep 1
  kill -0 "$server_pid" 2>/dev/null || { printf 'The HTTP server did not start; check config and logs.\n' >&2; return 1; }
  printf 'HTTPS with HTTP/2 (and HTTP/3 where UDP is available) is at https://localhost:8443. Ctrl-C stops both processes.\n'
  printf 'Caddy uses its local CA; run `caddy trust` if needed.\n'
  caddy run --config "$caddyfile" --adapter caddyfile
}

service_unit_command() {
  (($# <= 1)) || { printf 'Usage: manage.sh service-unit [config-file]\n' >&2; return 2; }
  local config_file="${1:-config/server.conf}" output_dir output_file
  [[ "$config_file" == /* ]] || config_file="$project_dir/$config_file"
  if [[ ! -s "$config_file" ]]; then
    local -a config_args=(init "$config_file")
    [[ ! -e "$config_file" ]] || config_args+=(--force)
    config_command "${config_args[@]}" || return
  fi
  build_project || return
  output_dir="$project_dir/build/deployment"
  output_file="$output_dir/http-server.service"
  mkdir -p "$output_dir" "$project_dir/data" "$project_dir/logs"
  cat > "$output_file" <<UNIT
[Unit]
Description=Configurable C HTTP and print server
Wants=network-online.target
After=network-online.target cups.service

[Service]
Type=simple
WorkingDirectory=$project_dir
ExecStart=$project_dir/build/http_server $config_file
LimitNOFILE=65536
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=$project_dir/data $project_dir/logs

[Install]
WantedBy=default.target
UNIT
  chmod 600 "$output_file"
  printf 'Generated user service unit: %s\n' "$output_file"
  printf 'Review paths and writable directories, then install it with:\n'
  printf '  mkdir -p ~/.config/systemd/user\n'
  printf '  install -m 600 %q ~/.config/systemd/user/http-server.service\n' "$output_file"
  printf '  systemctl --user daemon-reload && systemctl --user enable --now http-server.service\n'
}

static_app_name_valid() {
  [[ "$1" =~ ^[a-z][a-z0-9-]{0,47}$ ]]
}

static_command() {
  local action="${1:-}"; shift || true
  local apps_dir="$project_dir/public/apps"
  local config_file="$project_dir/config/server.conf"
  case "$action" in
    build|add|list|remove)
      if [[ ! -s "$config_file" ]]; then
        local -a config_args=(init "$config_file")
        [[ ! -e "$config_file" ]] || config_args+=(--force)
        config_command "${config_args[@]}" || return
      fi
      ;;
  esac
  case "$action" in
    build|add)
      (($# >= 2 && $# <= 3)) || {
        printf 'Usage: manage.sh static %s NAME %s [https://your-domain]\n' \
          "$action" "$([[ $action == build ]] && printf SOURCE_DIR || printf DIST_DIR)" >&2
        return 2
      }
      local name="$1" input_dir="$2" public_origin="${3:-}"
      local source_dir temp_dir final_dir config_tmp backup_dir mount_line origin_display
      static_app_name_valid "$name" || {
        printf 'App name must start with a lowercase letter and contain only lowercase letters, digits, and hyphens (max 48 chars).\n' >&2
        return 2
      }
      if [[ -n "$public_origin" ]]; then
        [[ "$public_origin" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?$ ]] || {
          printf 'Public origin must be an HTTPS origin such as https://print.example.com.\n' >&2
          return 2
        }
        origin_display="$public_origin"
      else
        origin_display='https://<your-domain>'
      fi
      [[ -d "$input_dir" ]] || { printf 'Directory not found: %s\n' "$input_dir" >&2; return 1; }
      source_dir="$(cd -- "$input_dir" && pwd -P)"
      if [[ "$action" == build ]]; then
        ensure_node_dependencies || return
        [[ -f "$source_dir/package.json" ]] || { printf 'Vite source must contain package.json: %s\n' "$source_dir" >&2; return 1; }
        if [[ -f "$source_dir/package-lock.json" || -f "$source_dir/npm-shrinkwrap.json" ]]; then
          (cd "$source_dir" && npm ci && npm run build -- "--base=/$name/") || return
        else
          printf 'Refusing a non-reproducible dependency install: commit package-lock.json or npm-shrinkwrap.json first.\n' >&2
          return 1
        fi
        source_dir="$source_dir/dist"
      fi
      [[ -d "$source_dir" && -f "$source_dir/index.html" ]] || {
        printf 'Build directory must contain index.html: %s\n' "$source_dir" >&2
        return 1
      }
      [[ -z "$(find "$source_dir" -type l -print -quit)" ]] || {
        printf 'Static build contains symbolic links; remove them before publishing.\n' >&2
        return 1
      }
      mkdir -p "$apps_dir"
      final_dir="$apps_dir/$name"
      mount_line="static_mount = /$name|apps/$name|spa"
      if [[ "$action" == add && ( -e "$final_dir" || "$(grep -Fxc "$mount_line" "$config_file" || true)" != 0 ) ]]; then
        printf 'Static app already exists: %s. Use static build to replace its deployment.\n' "$name" >&2
        return 1
      fi
      if [[ "$action" == build ]]; then
        if [[ -e "$final_dir" ]] && [[ "$(grep -Fxc "$mount_line" "$config_file" || true)" != 1 ]]; then
          printf 'Existing app directory has no matching config mount; refusing to overwrite it.\n' >&2
          return 1
        fi
        if [[ ! -e "$final_dir" ]] && [[ "$(grep -Fxc "$mount_line" "$config_file" || true)" != 0 ]]; then
          printf 'Config has a mount for %s but its installed directory is missing. Repair it with static add.\n' "$name" >&2
          return 1
        fi
      fi
      temp_dir="$(mktemp -d "$apps_dir/.${name}.XXXXXX")"
      config_tmp=""
      if ! cp -R "$source_dir/." "$temp_dir/" ||
         [[ ! -f "$temp_dir/index.html" ]] ||
         [[ -n "$(find "$temp_dir" -type l -print -quit)" ]]; then
        rm -rf -- "$temp_dir"; rm -f -- "$config_tmp"
        printf 'Could not validate the copied static build.\n' >&2
        return 1
      fi
      if [[ ! -e "$final_dir" ]]; then
        config_tmp="$(mktemp "$project_dir/config/.server.conf.XXXXXX")"
        if ! cat "$config_file" > "$config_tmp" ||
           ! printf '\n# Static SPA: %s\n%s\n' "$name" "$mount_line" >> "$config_tmp"; then
          rm -rf -- "$temp_dir"; rm -f -- "$config_tmp"; return 1
        fi
        chmod 600 "$config_tmp"
        if ! mv -- "$temp_dir" "$final_dir"; then
          rm -rf -- "$temp_dir"; rm -f -- "$config_tmp"; return 1
        fi
        if ! mv -- "$config_tmp" "$config_file"; then
          rm -rf -- "$final_dir"; rm -f -- "$config_tmp"; return 1
        fi
      else
        backup_dir="$(mktemp -d "$apps_dir/.${name}.backup.XXXXXX")"
        rmdir "$backup_dir"
        if ! mv -- "$final_dir" "$backup_dir"; then
          rm -rf -- "$temp_dir"; return 1
        fi
        if ! mv -- "$temp_dir" "$final_dir"; then
          mv -- "$backup_dir" "$final_dir" || true
          rm -rf -- "$temp_dir"; return 1
        fi
        rm -rf -- "$backup_dir"
      fi
      printf 'Installed %s at %s/%s/\n' "$name" "$origin_display" "$name"
      printf 'Static directory: %s\nConfiguration updated: %s\n' "$final_dir" "$config_file"
      printf 'Restart the server to load a new mount. Run static build again with a later Vite build to replace this deployment.\n'
      ;;
    list)
      (($# == 0)) || { printf 'Usage: manage.sh static list\n' >&2; return 2; }
      awk -F'[|=]' '/^[[:space:]]*static_mount[[:space:]]*=/ { path=$2; directory=$3; mode=$4; gsub(/[[:space:]]/, "", path); gsub(/[[:space:]]/, "", directory); gsub(/[[:space:]]/, "", mode); printf "https://<your-domain>%s/ -> public/%s (%s)\n", path, directory, mode }' "$config_file"
      ;;
    remove)
      (($# == 1)) || { printf 'Usage: manage.sh static remove NAME\n' >&2; return 2; }
      local name="$1" app_dir="$apps_dir/${1:-}" config_tmp answer
      static_app_name_valid "$name" || { printf 'Invalid static app name.\n' >&2; return 2; }
      grep -Fq "static_mount = /$name|apps/$name|spa" "$config_file" || {
        printf 'No configured static app named %s.\n' "$name" >&2; return 1
      }
      printf 'Remove the /%s/ mount and its deployed files at %s? [y/N] ' "$name" "$app_dir"
      IFS= read -r answer || answer=""
      [[ "$answer" == y || "$answer" == Y || "$answer" == yes || "$answer" == YES ]] || { printf 'Cancelled.\n'; return 0; }
      config_tmp="$(mktemp "$project_dir/config/.server.conf.XXXXXX")"
      awk -v expected="static_mount = /$name|apps/$name|spa" \
        '{ line=$0; sub(/^[[:space:]]+/, "", line); sub(/[[:space:]]+$/, "", line); if (line != expected) print $0 }' \
        "$config_file" > "$config_tmp"
      chmod 600 "$config_tmp"
      mv -- "$config_tmp" "$config_file" || return
      rm -rf -- "$app_dir"
      printf 'Removed %s. Restart the server to apply the change.\n' "$name"
      ;;
    -h|--help|help) sed -n '/^Static Vite applications:/,/^$/p' < <(usage) ;;
    *) printf 'Usage: manage.sh static {build NAME SOURCE_DIR [ORIGIN]|add NAME DIST_DIR [ORIGIN]|list|remove NAME}\n' >&2; return 2 ;;
  esac
}

interactive_menu() {
  local choice bind_address certificate_name port config_path action app_name app_dir
  while true; do
    local action_rc=0
    printf '\nConfigurable Web and Print Server setup\n'
    printf '  1) Guided local HTTPS setup and start\n'
    printf '  2) Generate a starter server config\n'
    printf '  3) Configure and build only\n'
    printf '  4) CUPS and printer setup\n'
    printf '  5) Publish a static/Vite app\n'
    printf '  6) Generate a development TLS certificate\n'
    printf '  7) Show command help\n'
    printf '  0) Exit\n'
    read -r -p 'Choose an option: ' choice || { printf '\n'; return 0; }
    case "$choice" in
      1)
        read -r -p 'Bind address [127.0.0.1]: ' bind_address || return 0
        read -r -p 'Certificate hostname [localhost]: ' certificate_name || return 0
        read -r -p 'HTTPS port [8443]: ' port || return 0
        bind_address="${bind_address:-127.0.0.1}"
        certificate_name="${certificate_name:-localhost}"
        port="${port:-8443}"
        run_local --bind "$bind_address" --name "$certificate_name" --port "$port" || action_rc=$?
        ;;
      2)
        printf 'Config profile:\n  1) Local development (127.0.0.1, HTTP)\n  2) Public server (0.0.0.0, HTTPS)\n'
        read -r -p 'Choose profile [1]: ' action || return 0
        case "${action:-1}" in
          1) action=local ;;
          2) action=public ;;
          *) printf 'Choose 1 (local) or 2 (public).\n' >&2; action_rc=2 ;;
        esac
        if ((action_rc == 0)); then
          read -r -p 'Output config [config/server.conf]: ' config_path || return 0
          config_command init "${config_path:-config/server.conf}" --profile "$action" || action_rc=$?
        fi
        ;;
      3)
        if build_project; then printf 'Build completed.\n'
        else action_rc=$?
        fi
        ;;
      4)
        printf '\nCUPS\n  1) Start CUPS\n  2) Show status\n  3) Discover printers\n  4) Add a driverless printer\n  0) Back\n'
        read -r -p 'Choose an option: ' action || return 0
        case "$action" in
          1) cups_command start || action_rc=$? ;;
          2) cups_command status || action_rc=$? ;;
          3) cups_command discover || action_rc=$? ;;
          4) cups_command add-printer || action_rc=$? ;;
          0) ;;
          *) printf 'Choose a listed option.\n' >&2 ;;
        esac
        ;;
      5)
        printf '\nStatic app\n  1) Build and publish a Vite app\n  2) Publish an existing build\n  3) List published apps\n  0) Back\n'
        read -r -p 'Choose an option: ' action || return 0
        case "$action" in
          1)
            read -r -p 'App name: ' app_name || return 0
            read -r -p 'Source directory: ' app_dir || return 0
            static_command build "$app_name" "$app_dir" || action_rc=$?
            ;;
          2)
            read -r -p 'App name: ' app_name || return 0
            read -r -p 'Build directory: ' app_dir || return 0
            static_command add "$app_name" "$app_dir" || action_rc=$?
            ;;
          3) static_command list || action_rc=$? ;;
          0) ;;
          *) printf 'Choose a listed option.\n' >&2 ;;
        esac
        ;;
      6)
        read -r -p 'Certificate hostname or IP [localhost]: ' certificate_name || return 0
        make_certificate "${certificate_name:-localhost}" 365 || action_rc=$?
        ;;
      7) usage || action_rc=$? ;;
      0) return 0 ;;
      *) printf 'Choose a listed option.\n' >&2 ;;
    esac
    if ((action_rc != 0)); then
      printf 'That action did not complete successfully (exit %d).\n' "$action_rc" >&2
    fi
  done
}

run_local() {
  local bind_address=127.0.0.1 certificate_name=localhost port=8443
  local with_cups=0 force_cert=0 check_only=0 start_only=0
  local base_config="$project_dir/config/server.conf" selected_config=""
  while (($#)); do
    case "$1" in
      --bind) (($# >= 2)) || { usage >&2; return 2; }; bind_address=$2; shift 2 ;;
      --name) (($# >= 2)) || { usage >&2; return 2; }; certificate_name=$2; shift 2 ;;
      --port) (($# >= 2)) || { usage >&2; return 2; }; port=$2; shift 2 ;;
      --config) (($# >= 2)) || { usage >&2; return 2; }; base_config=$2; shift 2 ;;
      --with-cups) with_cups=1; shift ;;
      --force-cert) force_cert=1; shift ;;
      --check) check_only=1; shift ;;
      --build) check_only=1; shift ;;
      --start) start_only=1; shift ;;
      -h|--help) usage; return 0 ;;
      *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; return 2 ;;
    esac
  done
  if [[ "$base_config" != /* ]]; then base_config="$project_dir/$base_config"; fi
  if ((start_only && (check_only || force_cert || with_cups))); then
    printf '%s\n' '--start cannot be combined with --check, --force-cert, or --with-cups.' >&2
    return 2
  fi
  if ((start_only)) &&
     [[ "$bind_address" != 127.0.0.1 || "$certificate_name" != localhost ||
        "$port" != 8443 ]]; then
    printf '%s\n' '--start uses the existing config; remove --bind, --name, and --port.' >&2
    return 2
  fi
  if ((start_only)); then
    selected_config="$base_config"
    if [[ "$selected_config" == "$project_dir/config/server.conf" &&
          -s "$project_dir/build/local-setup/server.conf" ]]; then
      selected_config="$project_dir/build/local-setup/server.conf"
    fi
    if [[ ! -s "$selected_config" ]]; then
      local -a config_args=(init "$selected_config")
      [[ ! -e "$selected_config" ]] || config_args+=(--force)
      config_command "${config_args[@]}" || return
      if grep -q '^# Generated profile: public\.' "$selected_config"; then
        printf 'Public config is ready at %s. Install a trusted TLS certificate and matching private key before starting the server.\n' "$selected_config"
        return 0
      fi
    fi
    [[ -x "$project_dir/build/http_server" ]] || {
      printf 'Server binary is missing. Run manage.sh check first.\n' >&2; return 1;
    }
    [[ -f "$selected_config" ]] || {
      printf 'Config file not found: %s\n' "$selected_config" >&2; return 1;
    }
    server_command --tls "$selected_config"
  fi
  if [[ ! -s "$base_config" ]]; then
    local -a config_args=(init "$base_config")
    [[ ! -e "$base_config" ]] || config_args+=(--force)
    config_command "${config_args[@]}" || return
  fi
  if grep -q '^# Generated profile: public\.' "$base_config"; then
    printf 'Public config is ready at %s. Install a trusted TLS certificate, edit its configured paths, then run: manage.sh server --native-tls %s\n' \
      "$base_config" "$base_config"
    return 0
  fi
  [[ "$port" =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || {
    printf 'Port must be an integer from 1 to 65535.\n' >&2; return 2; }
  [[ "$bind_address" =~ ^[A-Za-z0-9._:%-]+$ && -n "$certificate_name" && "$certificate_name" != *$'\n'* ]] || {
    printf 'Bind address and certificate name must be non-empty single-line values.\n' >&2; return 2; }
  if ((with_cups && !check_only)); then cups_start || return; fi
  build_project || return
  ((check_only)) && { printf 'Build and dependency checks completed.\n'; return 0; }
  local certificate="$project_dir/certs/server.crt" private_key="$project_dir/certs/server.key"
  if ((force_cert)); then make_certificate --force "$certificate_name" 365 || return
  elif [[ ! -e "$certificate" && ! -e "$private_key" ]]; then make_certificate "$certificate_name" 365 || return
  elif [[ ! -f "$certificate" || ! -f "$private_key" ]]; then
    printf 'Only part of the TLS certificate pair exists. Repair it with --force-cert.\n' >&2; return 1
  fi
  if ((!force_cert)); then
    require_command openssl || return
    if [[ "$certificate_name" == *:* || "$certificate_name" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
      openssl x509 -in "$certificate" -noout -checkip "$certificate_name" >/dev/null 2>&1 || {
        printf 'The existing certificate does not match %s. Regenerate it with --force-cert.\n' "$certificate_name" >&2
        return 1
      }
    else
      openssl x509 -in "$certificate" -noout -checkhost "$certificate_name" >/dev/null 2>&1 || {
        printf 'The existing certificate does not match %s. Regenerate it with --force-cert.\n' "$certificate_name" >&2
        return 1
      }
    fi
  fi
  local runtime_dir="$project_dir/build/local-setup" admin_token_file runtime_config runtime_tmp admin_token backend_port site_address caddyfile
  ensure_caddy_dependency || return
  backend_port=$((10#$port + 1))
  ((backend_port <= 65535)) || { printf 'The HTTPS port must be 65534 or lower to reserve the next port for the backend.\n' >&2; return 2; }
  if [[ "$certificate_name" == *:* ]]; then site_address="[$certificate_name]:$port"
  else site_address="$certificate_name:$port"
  fi
  mkdir -p "$runtime_dir" && chmod 700 "$runtime_dir" || return
  admin_token_file="$runtime_dir/admin.token"
  if [[ ! -s "$admin_token_file" ]]; then
    local token_tmp
    token_tmp="$(mktemp "$runtime_dir/.admin.token.XXXXXX")" || return
    if ! openssl rand -hex 32 > "$token_tmp" || ! chmod 600 "$token_tmp" ||
       ! mv -f -- "$token_tmp" "$admin_token_file"; then
      rm -f -- "$token_tmp"
      printf 'Could not generate admin credential.\n' >&2
      return 1
    fi
  fi
  chmod 600 "$admin_token_file" || return
  admin_token="$(<"$admin_token_file")"
  [[ "$admin_token" =~ ^[A-Fa-f0-9]{64}$ ]] || { printf 'Admin-token file is invalid: %s\n' "$admin_token_file" >&2; return 1; }
  runtime_config="$runtime_dir/server.conf"
  runtime_tmp="$(mktemp "$runtime_dir/.server.conf.XXXXXX")" || return
  if ! {
    # Config parsing applies the last value, allowing setup values to override
    # the base file while preserving all of its application settings.
    cat "$base_config"
    cat <<CONFIG

# Generated by scripts/manage.sh. Keep this file private.
host = 127.0.0.1
port = $backend_port
tls_enabled = true
tls_certificate_file = $certificate
tls_private_key_file = $private_key

admin_api_token = $admin_token
CONFIG
  } > "$runtime_tmp"; then
    rm -f -- "$runtime_tmp"
    printf 'Could not generate runtime config: %s\n' "$runtime_config" >&2
    return 1
  fi
  if ! chmod 600 "$runtime_tmp" || ! mv -f -- "$runtime_tmp" "$runtime_config"; then
    rm -f -- "$runtime_tmp"
    printf 'Could not install runtime config: %s\n' "$runtime_config" >&2
    return 1
  fi
  printf 'Starting HTTPS at https://%s:%s/\n' "$certificate_name" "$port"
  printf 'Admin credential is stored in %s (mode 0600).\n' "$admin_token_file"
  printf 'Caddy terminates browser TLS with HTTP/2; HTTP/3 is available when UDP is reachable.\n'
  printf "Trust Caddy's local CA with `caddy trust` if needed.\n"
  raise_open_file_limit
  caddyfile="$runtime_dir/Caddyfile"
  cat > "$caddyfile" <<CADDY
{
  admin off
}

$site_address {
  bind $bind_address
  tls internal
  reverse_proxy https://127.0.0.1:$backend_port {
    transport http {
      tls_insecure_skip_verify
    }
  }
}
CADDY
  chmod 600 "$caddyfile"
  "$project_dir/build/http_server" "$runtime_config" &
  local server_pid=$!
  cleanup_local_server() { local result=$?; trap - EXIT INT TERM; kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; exit "$result"; }
  trap cleanup_local_server EXIT INT TERM
  sleep 1
  kill -0 "$server_pid" 2>/dev/null || { printf 'The HTTP server did not start; check config and logs.\n' >&2; return 1; }
  caddy run --config "$caddyfile" --adapter caddyfile
}

cd "$project_dir"
case "${1:-}" in
  -h|--help) usage ;;
  menu) shift; (($# == 0)) || { usage >&2; exit 2; }; interactive_menu ;;
  config) shift; config_command "$@" ;;
  cert) shift; make_certificate "$@" ;;
  cups) shift; cups_command "$@" ;;
  static) shift; static_command "$@" ;;
  server) shift; server_command "$@" ;;
  service-unit) shift; service_unit_command "$@" ;;
  check) shift; (($# == 0)) || { usage >&2; exit 2; }; build_project ;;
  *)
    if (($# == 0)) && [[ -t 0 ]]; then interactive_menu
    else run_local "$@"
    fi
    ;;
esac
