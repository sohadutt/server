#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: generate_cert.sh [--force] [hostname-or-ip] [valid-days]

Creates a self-signed development certificate in certs/dev-server.crt and
certs/dev-server.key. The certificate includes a Subject Alternative Name.
Existing files are preserved unless --force is supplied.
EOF
}

force=0
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi
if [[ "${1:-}" == "--force" ]]; then
  force=1
  shift
fi
if [[ "$#" -gt 2 ]]; then
  usage >&2
  exit 2
fi

hostname="${1:-localhost}"
days="${2:-365}"
if [[ ! "$days" =~ ^[1-9][0-9]*$ ]] || (( ${#days} > 3 )) ||
   (( 10#$days > 825 )); then
  printf 'Certificate validity must be between 1 and 825 days.\n' >&2
  exit 2
fi

if ! command -v openssl >/dev/null 2>&1; then
  printf 'OpenSSL is required. Install it with your system package manager and retry.\n' >&2
  exit 1
fi

if [[ "$hostname" == *:* || "$hostname" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  san="IP:$hostname"
elif [[ "$hostname" =~ ^[A-Za-z0-9*.-]+$ ]]; then
  san="DNS:$hostname"
else
  printf 'Use a DNS hostname or IP address for the certificate SAN.\n' >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
cert_dir="$project_dir/certs"
key_file="$cert_dir/server.key"
cert_file="$cert_dir/server.crt"
mkdir -p "$cert_dir"

if (( ! force )) && { [[ -e "$key_file" ]] || [[ -e "$cert_file" ]]; }; then
  printf 'Certificate or key already exists. Use --force to replace it.\n' >&2
  exit 1
fi

umask 077
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -days "$days" \
  -subj '/CN=Development Certificate' \
  -addext "subjectAltName=$san" \
  -keyout "$key_file" -out "$cert_file"
chmod 600 "$key_file"
chmod 644 "$cert_file"

printf 'Created development certificate for %s (%s days).\n' "$hostname" "$days"
printf 'Certificate: %s\nPrivate key: %s\n' "$cert_file" "$key_file"
printf 'This server does not load these files directly; configure a TLS proxy to use them.\n'
