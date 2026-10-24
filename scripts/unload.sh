#!/usr/bin/env bash
# Remove the module. Fails with "Module vaccel is in use" while any process
# still has /dev/vaccel0 open or mapped, which is the intended behaviour.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
rmmod vaccel
dmesg | grep vaccel | tail -n 1
