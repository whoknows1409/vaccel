#!/usr/bin/env bash
# Boot a kernel in QEMU, load vaccel.ko, run the test suite and benchmark,
# reload the module with different parameters and test again, unload it, then
# scan the kernel log for warnings, lockdep reports, KASAN/UBSAN errors and
# kmemleak leaks. The host kernel is never touched.
#
#   KERNEL=/path/to/bzImage KDIR=/path/to/matching/kernel/tree scripts/qemu-test.sh
#
# For the strongest check, use a kernel built with CONFIG_PROVE_LOCKING,
# CONFIG_KASAN, CONFIG_DEBUG_KMEMLEAK and CONFIG_DEBUG_ATOMIC_SLEEP.
set -euo pipefail

KERNEL=${KERNEL:?set KERNEL to a bzImage}
KDIR=${KDIR:?set KDIR to the build tree matching KERNEL}
MEM=${MEM:-2G}
TIMEOUT=${TIMEOUT:-1800}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== building module against $KDIR"
make -s -C "$ROOT/kernel" KDIR="$KDIR" >/dev/null
echo "== building static user-space binaries"
make -s -C "$ROOT/user" STATIC=1 B=build-static \
	build-static/test_vaccel build-static/bench_vaccel build-static/vaccelctl >/dev/null

R="$WORK/root"
mkdir -p "$R"/{bin,proc,sys,dev,tmp}
cp "$(command -v busybox)" "$R/bin/busybox"
for app in sh mount insmod rmmod lsmod cat echo grep sed dmesg poweroff sleep ls tail; do
	ln -s busybox "$R/bin/$app"
done
cp "$ROOT/kernel/vaccel.ko" "$R/"
cp "$ROOT"/user/build-static/{test_vaccel,bench_vaccel,vaccelctl} "$R/"

cat > "$R/init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mount -t debugfs none /sys/kernel/debug
rc=0
# Show which runtime checkers this kernel has, then start from an empty log.
dmesg | grep -oE "KernelAddressSanitizer initialized.*|kmemleak: Kernel memory leak detector initialized.*" || true
dmesg -c > /dev/null

run_suite() {
	echo "== insmod vaccel.ko $*"
	insmod /vaccel.ko "$@" || { echo "insmod failed"; rc=1; return; }
	/vaccelctl demo || rc=1
	/test_vaccel || rc=1
	/bench_vaccel --threads 4 --seconds 2 --size 262144 || rc=1
	/vaccelctl info
	echo "-- buddyinfo"; /vaccelctl buddy
	echo "== rmmod vaccel"
	rmmod vaccel || rc=1
}

run_suite vram_mb=64 max_channels=32
run_suite vram_mb=48 max_channels=4 sched_quantum=1 max_queue_depth=64

if [ -e /sys/kernel/debug/kmemleak ]; then
	echo "== kmemleak scan"
	echo scan > /sys/kernel/debug/kmemleak; sleep 6
	echo scan > /sys/kernel/debug/kmemleak
	cat /sys/kernel/debug/kmemleak > /tmp/leaks
	if [ -s /tmp/leaks ]; then echo "kmemleak reports:"; cat /tmp/leaks; rc=1; else echo "no leaks"; fi
fi

# Prove the debug checkers were live for the whole run (debug kernels only).
if [ -e /proc/lockdep_stats ]; then
	grep "debug_locks:" /proc/lockdep_stats
	grep -q "debug_locks: *1" /proc/lockdep_stats || { echo "lockdep switched itself off"; rc=1; }
fi

echo "== kernel log since boot (vaccel + any splats)"
dmesg | sed "s/^\[[^]]*\] //" | grep -E "vaccel|BUG|WARNING|circular|lockdep|KASAN|UBSAN|unreferenced|Call Trace|sleeping function" > /tmp/log || true
cat /tmp/log
if grep -qE "BUG|WARNING|circular|KASAN|UBSAN|Call Trace|sleeping function" /tmp/log; then
	echo "kernel reported problems"; rc=1
fi
echo "QEMU_RESULT rc=$rc"
poweroff -f
EOF
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip -1 > "$WORK/initrd.gz")

ACCEL=""
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
echo "== booting $(basename "$KERNEL") in QEMU ${ACCEL:+(KVM)}"
timeout --foreground "$TIMEOUT" qemu-system-x86_64 $ACCEL -m "$MEM" -smp 2 -nographic -no-reboot \
	-kernel "$KERNEL" -initrd "$WORK/initrd.gz" \
	-append "console=ttyS0 quiet panic=-1 loglevel=4" </dev/null 2>&1 | tee "$WORK/serial.log" | \
	grep -vE "^\[ *[0-9]+\.[0-9]+\]|^$" || true

grep -q "QEMU_RESULT rc=0" "$WORK/serial.log" && { echo "== PASS"; exit 0; }
echo "== FAIL"
exit 1
