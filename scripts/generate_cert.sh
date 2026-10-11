#!/usr/bin/env bash

set -Eeuo pipefail

DOMAIN="${1:-localhost}"
DAYS="${2:-365}"
KEY_FILE="certs/server.key"
CSR_FILE="certs/server.csr"
CRT_FILE="certs/server.crt"

usage() {
    echo "Usage: $0 [domain] [days]"
    exit 1
}

if [[ "$#" -gt 2 || -z "$DOMAIN" || ! "$DAYS" =~ ^[1-9][0-9]*$ ]]; then
    usage
fi

install_openssl() {
    echo "OpenSSL not found. Installing..."

    if [[ "$OSTYPE" == "darwin"* ]]; then
        command -v brew >/dev/null 2>&1 || {
            echo "Error: Install Homebrew from https://brew.sh/"
            exit 1
        }
        brew install openssl
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y openssl
    elif command -v yum >/dev/null 2>&1; then
        sudo yum install -y openssl
    elif command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y openssl
    elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -Sy --needed --noconfirm openssl
    elif command -v apk >/dev/null 2>&1; then
        sudo apk add openssl
    else
        echo "Error: Unsupported package manager."
        exit 1
    fi

    command -v openssl >/dev/null 2>&1 || {
        echo "Error: OpenSSL installation failed or is not in PATH."
        exit 1
    }
}

if ! command -v openssl >/dev/null 2>&1; then
    install_openssl
fi

echo "OpenSSL: $(openssl version)"
echo "Generating certificate for $DOMAIN ($DAYS days)..."

mkdir -p certs

openssl genrsa -out "$KEY_FILE" 2048

openssl req -new -key "$KEY_FILE" -out "$CSR_FILE" \
    -subj "/C=US/ST=State/L=City/O=Organization/CN=$DOMAIN"

openssl x509 -req -days "$DAYS" -in "$CSR_FILE" \
    -signkey "$KEY_FILE" -out "$CRT_FILE" -sha256

echo "Certificate generation completed."
echo "Private key: $KEY_FILE"
echo "CSR:         $CSR_FILE"
echo "Certificate: $CRT_FILE"
