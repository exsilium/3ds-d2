#!/bin/sh
# The TLS module test (tls_test.c) in the host-test image: builds it, starts
# a local TLS server with a self-signed certificate for the refusal case,
# and runs both cases under qemu-arm. Needs build/cpu/ctr/tls.bin and the
# network (one test event goes to the project in packaging/cia/report.dsn).
set -eu
cd /work
out=build/tls-test
mkdir -p "$out"
arm-linux-gnueabi-gcc -std=gnu17 -O1 -Wall -Wextra -Werror -march=armv6k -marm -static tests/tls/tls_test.c -o "$out/tls_test"
dsn="$(tr -d '\r\n' < packaging/cia/report.dsn)"
key="${dsn#https://}"; key="${key%%@*}"
host="${dsn#*@}"; host="${host%%/*}"
project="${dsn##*/}"
ip="$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$host")"
openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=localhost -days 1 -keyout "$out/key.pem" -out "$out/cert.pem" 2>/dev/null
openssl s_server -quiet -accept 18443 -key "$out/key.pem" -cert "$out/cert.pem" -www > /dev/null 2>&1 &
server=$!
trap 'kill $server 2>/dev/null' EXIT
sleep 1
qemu-arm -cpu arm11mpcore "$out/tls_test" build/cpu/ctr/tls.bin "$ip" "$host" "/api/$project/envelope/" "$key" 18443
