#!/usr/bin/env bash
# Insert vaccel.ko into the running kernel. Extra arguments are module
# parameters, for example:  scripts/load.sh vram_mb=128 max_channels=16
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

KO=kernel/vaccel.ko
[ -f "$KO" ] || { echo "kernel/vaccel.ko not found: run 'make' first"; exit 1; }
if lsmod | grep -q '^vaccel '; then rmmod vaccel; fi

if ! insmod "$KO" "$@"; then
	echo "insmod failed. If dmesg says 'Key was rejected by service', Secure Boot"
	echo "is on: sign the module with your MOK key or test in QEMU (make qemu-test)."
	exit 1
fi

for _ in $(seq 50); do [ -e /dev/vaccel0 ] && break; sleep 0.1; done
chmod 0666 /dev/vaccel0   # development convenience; tests still need root for sysfs
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
dmesg | grep vaccel | tail -n 1
