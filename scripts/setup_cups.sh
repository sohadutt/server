#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: setup_cups.sh [--start | --status | --discover | --add-printer [args...]]

  --start        Start and enable the CUPS scheduler, then show queues (default)
  --status       Show scheduler and configured queue status
  --discover     List discovered printer URIs without adding a queue
  --add-printer  Run add_printer.sh to create a driverless CUPS queue
EOF
}

action="--start"
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi
if [[ "${1:-}" == --* ]]; then
  action="$1"
  shift
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"

run_privileged() {
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    printf 'This CUPS service operation needs administrator privileges.\n' >&2
    return 1
  fi
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Required CUPS command not found: %s\n' "$1" >&2
    printf 'Install CUPS with your operating system package manager first.\n' >&2
    exit 1
  fi
}

show_status() {
  lpstat -r
  printf '\nConfigured printers and classes:\n'
  lpstat -p -d 2>/dev/null || true
}

case "$action" in
  --status)
    if [[ "$#" -ne 0 ]]; then usage >&2; exit 2; fi
    require_command lpstat
    show_status
    ;;
  --discover)
    if [[ "$#" -ne 0 ]]; then usage >&2; exit 2; fi
    require_command lpinfo
    lpinfo -v | awk '$1 == "network" && $2 ~ /^(ipp|ipps|dnssd):\/\// { print $2 }'
    ;;
  --add-printer)
    "$project_dir/scripts/add_printer.sh" "$@"
    ;;
  --start)
    if [[ "$#" -ne 0 ]]; then usage >&2; exit 2; fi
    require_command lpstat
    case "$(uname -s)" in
      Linux)
        if command -v systemctl >/dev/null 2>&1; then
          if systemctl cat cups.service >/dev/null 2>&1; then
            run_privileged systemctl enable --now cups.service
          elif systemctl cat cups.socket >/dev/null 2>&1; then
            run_privileged systemctl enable --now cups.socket
          else
            printf 'Could not find cups.service or cups.socket in systemd.\n' >&2
            exit 1
          fi
        elif command -v service >/dev/null 2>&1; then
          run_privileged service cups start
        else
          printf 'Could not find systemctl or service to start CUPS.\n' >&2
          exit 1
        fi
        ;;
      Darwin)
        require_command launchctl
        plist=/System/Library/LaunchDaemons/org.cups.cupsd.plist
        if [[ ! -f "$plist" ]]; then
          printf 'CUPS launch daemon not found: %s\n' "$plist" >&2
          exit 1
        fi
        run_privileged launchctl enable system/org.cups.cupsd
        if ! launchctl print system/org.cups.cupsd >/dev/null 2>&1; then
          run_privileged launchctl bootstrap system "$plist"
        fi
        run_privileged launchctl kickstart system/org.cups.cupsd
        ;;
      *)
        printf 'Automatic CUPS service setup is supported on Linux and macOS.\n' >&2
        printf 'Start CUPS with your OS service manager, then use --status.\n' >&2
        exit 1
        ;;
    esac
    show_status
    printf '\nTo discover printers: %s --discover\n' "$0"
    printf 'To add a driverless queue: %s --add-printer\n' "$0"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
