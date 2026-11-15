#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the minimal chopin bring-up initramfs (a static PID 1 that proves the
# kernel reached userspace on the serial console).
#
# Usage: tools/mkinitramfs.sh [output-dir]
#
# INITRAMFS_MODE=rootfs  -> minimal PID 1 that mounts the distro and switch_roots
# INITRAMFS_MODE=bringup -> Alpine userland, diagnostics and a USB shell (default)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
port=$(dirname "$here")
ws=$(dirname "$port")
out=${1:-"$port/out"}

# INITRAMFS_MODE=rootfs builds the other shape: no userland at all, just a
# static PID 1 that mounts the distro from /dev/sdc75 (or chopin.root=) and
# switch_roots into it.  That is what you want once the distro lives on
# userdata - the ramdisk should carry no Alpine, no shell and no diagnostics,
# because there is nothing left to rescue onto: the console is the distro's.
mode=${INITRAMFS_MODE:-bringup}

cc="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
root="$out/initramfs-root"

command -v "$cc" >/dev/null || { echo "error: $cc not found" >&2; exit 1; }

rm -rf "$root"
mkdir -p "$root"

# Alpine userland: a real rootfs (busybox, musl, apk) instead of the tiny
# static shell, so the board can run standard tools.  Copy it before our own
# /init so that whatever the distro ships as /init cannot win.
if [ "$mode" = bringup ] && [ -d "$port/rootfs-alpine" ]; then
    cp -a "$port/rootfs-alpine/." "$root/"
fi

mkdir -p "$root"/{bin,dev,proc,sys,tmp}
mkdir -p "$root/lib/firmware"
chmod 0755 "$root"

# Board firmware that must exist *before* the driver initcalls run.  The
# vendor conninfra.cfg is 17 bytes but decides the co-clock scheme: without
# it mt6893's consys_co_clock_type_mt6893() returns -1 ("Get conf fail") and
# the RF clock setup is skipped.  /vendor is only mounted much later by
# fwload, so this one has to live in the cpio.
if [ -d "$port/firmware/chopin-wifi" ]; then
    cp -a "$port/firmware/chopin-wifi/." "$root/lib/firmware/"
fi

# Optional userspace GPU driver (a G77-capable libgallium-*.so).  It is
# shipped inside the initramfs and dropped into the distro's /root by the
# init so it can be tried with LD_LIBRARY_PATH without reflashing userdata.
if [ -n "${GPU_DRIVER:-}" ] && [ -f "$GPU_DRIVER" ]; then
    install -m 0644 "$GPU_DRIVER" "$root/lib/firmware/$(basename "$GPU_DRIVER")"
    echo "initramfs: GPU driver from $GPU_DRIVER"
fi

# init.c lives at the top of this repository (it used to sit in initramfs/).
init_src=${INIT_SRC:-"$port/init.c"}
"$cc" -static -Os -Wall -Wextra -o "$root/init" "$init_src"
chmod 0755 "$root/init"

# Optional module tree (for testing drivers as modules after boot).
if [ -n "${MODULES_DIR:-}" ] && [ -d "$MODULES_DIR/lib/modules" ]; then
    mkdir -p "$root/lib"
    cp -a "$MODULES_DIR/lib/modules" "$root/lib/"
    echo "initramfs: modules from $MODULES_DIR"
fi

if [ "$mode" = bringup ]; then
# Standalone helper for the Alpine userland (the old in-init shell command was
# only reachable from the built-in shell).
mkdir -p "$root/usr/bin"
"$cc" -static -Os -o "$root/usr/bin/wifiscan" "$port/tools/wifiscan.c"
chmod 0755 "$root/usr/bin/wifiscan"

# devmem(1): busybox in the ramdisk has no such applet, and MMIO pokes are
# how the port checks blocks (SCP registers, audio top registers, ...) that
# no driver exposes yet.
"$cc" -static -Os -Wall -o "$root/usr/bin/devmem" "$port/tools/devmem.c"
chmod 0755 "$root/usr/bin/devmem"

# Recovery UI (separate project in ../recovery): ship it when it has been built.
if [ -x "$ws/recovery/build/recovery.stripped" ]; then
    mkdir -p "$root/usr/bin"
    install -m 0755 "$ws/recovery/build/recovery.stripped" "$root/usr/bin/recovery"
    echo "initramfs: recovery UI from ../recovery"
fi
if [ -x "$ws/recovery/rootfs/usr/bin/recovery-ctl" ]; then
    mkdir -p "$root/usr/bin"
    install -m 0755 "$ws/recovery/rootfs/usr/bin/recovery-ctl" "$root/usr/bin/recovery-ctl"
fi

# Always overlay the tracked helper, even when rootfs-alpine was built earlier.
if [ -f "$port/tools/wifi-connect.sh" ]; then
    mkdir -p "$root/usr/bin" "$root/usr/local/bin"
    install -m 0755 "$port/tools/wifi-connect.sh" "$root/usr/bin/wifi-connect"
    install -m 0755 "$port/tools/wifi-connect.sh" "$root/usr/local/bin/wifi-connect"
fi

# Board-side SCP bring-up diagnostic (see port/AUTO-ROTATION-STATUS.md):
#   sh /usr/bin/board-scp-audit.sh [start|start2]
if [ -f "$port/tools/board-scp-audit.sh" ]; then
    mkdir -p "$root/usr/bin"
    install -m 0755 "$port/tools/board-scp-audit.sh" \
        "$root/usr/bin/board-scp-audit.sh"
fi

fi

# Optional: point the init at a real root filesystem on a partition (the
# mt6895 model - distro on userdata, initramfs only carries the board glue).
if [ -n "${CHOPIN_RECOVERY:-}" ]; then
    mkdir -p "$root/etc/chopin"
    : > "$root/etc/chopin/recovery"
    echo "initramfs: recovery marker (boots straight into the UI)"
fi

# Don't send any control packet to the SCP: the user's 10-chsensor.start hook
# turns chsensor_zero_ipi on before it releases the core.  This has to be baked
# into the ramdisk rather than set at runtime, because the SCP is released at
# boot and a mid-session patchscp/fullreset/sram/start does not bring it back
# (measured: second instance stops logging at t=0.082 s and
# /sys/class/misc/scp/scp_A_status stays "not ready").
if [ -n "${CHOPIN_ZERO_IPI:-}" ]; then
    mkdir -p "$root/etc/chopin"
    : > "$root/etc/chopin/zero-ipi"
    echo "initramfs: zero-IPI marker (no SCP control packets at all)"
fi

if [ -n "${CHOPIN_ROOTFS:-}" ]; then
    mkdir -p "$root/etc/chopin"
    printf '%s\n' "$CHOPIN_ROOTFS" > "$root/etc/chopin/rootfs"
    echo "initramfs: rootfs device $CHOPIN_ROOTFS"
fi

mkdir -p "$out"
( cd "$root" && find . -print0 |
    cpio --null -o --format=newc --quiet |
    gzip -9 ) > "$out/initramfs.cpio.gz"

echo "initramfs: $out/initramfs.cpio.gz ($(stat -c %s "$out/initramfs.cpio.gz") bytes)"
