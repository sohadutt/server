#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: add_printer.sh [queue-name] [printer-uri] [description]

Without a printer URI, the script lists discovered IPP destinations and lets
you select one. It then creates a driverless CUPS queue with lpadmin.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ "$#" -gt 3 ]]; then
  usage >&2
  exit 2
fi

for command_name in lpinfo lpadmin lpstat awk; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'Required command not found: %s\n' "$command_name" >&2
    exit 1
  fi
done

queue_name="${1:-}"
printer_uri="${2:-}"
description="${3:-}"

if [[ -z "$printer_uri" ]]; then
  candidates=()
  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] && candidates+=("$candidate")
  done < <(lpinfo -v | awk '$1 == "network" { print $2 }' | awk '/^(ipp|ipps|dnssd):\/\//')

  if [[ "${#candidates[@]}" -eq 0 ]]; then
    printf 'No IPP destinations were discovered. Enter an ipp:// or ipps:// URI: '
    IFS= read -r printer_uri
  else
    printf 'Discovered IPP destinations:\n'
    select candidate in "${candidates[@]}" 'Enter a URI manually'; do
      if [[ -n "${candidate:-}" && "$candidate" == 'Enter a URI manually' ]]; then
        printf 'Printer URI: '
        IFS= read -r printer_uri
        break
      elif [[ -n "${candidate:-}" ]]; then
        printer_uri="$candidate"
        break
      else
        printf 'Choose a listed number.\n' >&2
      fi
    done
  fi
fi

if [[ ! "$printer_uri" =~ ^(ipp|ipps|dnssd)://[^[:space:]]+$ ]]; then
  printf 'Unsupported or invalid printer URI: %s\n' "$printer_uri" >&2
  printf 'Use a discovered ipp://, ipps://, or dnssd:// URI.\n' >&2
  exit 2
fi

if [[ -z "$queue_name" ]]; then
  printf 'CUPS queue name: '
  IFS= read -r queue_name
fi
if [[ ! "$queue_name" =~ ^[A-Za-z0-9_.-]+$ ]]; then
  printf 'Queue names may contain only letters, digits, dot, underscore, and hyphen.\n' >&2
  exit 2
fi

if [[ -z "$description" ]]; then
  printf 'Description (optional): '
  IFS= read -r description
fi

if lpstat -p "$queue_name" >/dev/null 2>&1; then
  printf 'CUPS queue already exists: %s\n' "$queue_name" >&2
  printf 'Choose another name to avoid changing an existing queue.\n' >&2
  exit 1
fi

if [[ "${EUID:-$(id -u)}" -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then
  printf 'Adding a queue requires root or sudo.\n' >&2
  exit 1
fi

printf 'Adding queue %s for %s\n' "$queue_name" "$printer_uri"
lpadmin_args=(-p "$queue_name" -E -v "$printer_uri" -m everywhere)
if [[ -n "$description" ]]; then
  lpadmin_args+=(-D "$description")
fi

if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
  lpadmin "${lpadmin_args[@]}"
else
  sudo lpadmin "${lpadmin_args[@]}"
fi

lpstat -p "$queue_name" -l
printf 'Added CUPS queue: %s\n' "$queue_name"
