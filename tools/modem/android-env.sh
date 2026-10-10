#!/bin/sh
# Minimal Android environment for running Samsung's own CP boot daemon
# (cbd) on postmarketOS. No Android services are started: cbd only needs the
# bionic linker, liblog/libcutils/libc++ and the modem device nodes.
#
# Two sources for bionic:
#  - a prebuilt root unpacked into $ROOT (tools/modem/make-android-root.sh:
#    TWRP's bionic + cbd) - used when $ROOT/system/bin/linker64 exists;
#    this is the tested path (the dev phone's SYSTEM partition is empty)
#  - otherwise the phone's SYSTEM partition (stock or LineageOS)
#
#   android-env.sh up      mount everything under $ROOT (idempotent)
#   android-env.sh down    unmount
#   android-env.sh run CMD run CMD inside the environment (as root)
#
# Layout inside $ROOT (default /var/lib/android-modem):
#   /system   SYSTEM partition (read-only); /vendor -> system/vendor
#   /apex/*   bionic runtime APEX payloads (Android 10+ only)
#   /efs, /mnt/vendor/efs   EFS partition (read-write: NV data lives here)
#   /dev, /proc, /sys       bind mounts
#   /dev/block/platform/11120000.ufs/by-name/RADIO -> RADIO partition
set -eu

ROOT=${ROOT:-/var/lib/android-modem}
BYLABEL=/dev/disk/by-partlabel

die() { echo "android-env: $*" >&2; exit 1; }

is_mounted() { grep -q " $1 " /proc/mounts; }

mnt() {	# mnt <opts> <src> <dst>
	mkdir -p "$3"
	is_mounted "$3" || mount $1 "$2" "$3"
}

up() {
	[ -e "$BYLABEL/EFS" ] || die "no EFS partition"
	[ -e "$BYLABEL/RADIO" ] || die "no RADIO partition"
	mkdir -p "$ROOT"

	if [ -x "$ROOT/system/bin/linker64" ] && ! is_mounted "$ROOT/system"; then
		: # prebuilt root (make-android-root.sh)
	else
	[ -e "$BYLABEL/SYSTEM" ] || die "no SYSTEM partition and no prebuilt root"
	# SYSTEM: either a classic /system image or system-as-root
	mnt "-o ro" "$BYLABEL/SYSTEM" "$ROOT/.sysimg"
	if [ -d "$ROOT/.sysimg/system/bin" ]; then
		sys="$ROOT/.sysimg/system"
	else
		sys="$ROOT/.sysimg"
	fi
	mnt "--bind -o ro" "$sys" "$ROOT/system"
	[ -e "$ROOT/vendor" ] || ln -s system/vendor "$ROOT/vendor"

	# Android 10+: bionic lives in the runtime APEX. Mount the ext4
	# payloads of the APEXes the linker needs.
	for apex in "$ROOT"/system/apex/com.android.runtime*.apex \
		    "$ROOT"/system/apex/com.android.art*.apex; do
		[ -f "$apex" ] || continue
		name=$(basename "$apex" .apex)
		name=${name%.release}; name=${name%.debug}
		img="$ROOT/.apex/$name.img"
		mkdir -p "$ROOT/.apex"
		[ -f "$img" ] || unzip -p "$apex" apex_payload.img > "$img"
		mnt "-o ro,loop" "$img" "$ROOT/apex/$name"
	done
	fi

	# EFS: read-write, cbd writes nv_data.bin back after the CP updates it
	mnt "" "$BYLABEL/EFS" "$ROOT/efs"
	mnt "--bind" "$ROOT/efs" "$ROOT/mnt/vendor/efs"

	mnt "--rbind" /dev "$ROOT/dev"
	mnt "-t proc" proc "$ROOT/proc"
	mnt "--rbind" /sys "$ROOT/sys"
	mkdir -p "$ROOT/data/vendor/log/cbd" "$ROOT/tmp"

	# cbd opens /dev/block/<-P argument>; give it the stock path
	mkdir -p /dev/block/platform/11120000.ufs/by-name
	ln -sf "$(readlink -f "$BYLABEL/RADIO")" \
		/dev/block/platform/11120000.ufs/by-name/RADIO
	echo "android-env: up at $ROOT"
}

down() {
	for m in $(awk -v r="$ROOT" '$2 ~ "^"r {print $2}' /proc/mounts | sort -r); do
		umount -l "$m" || true
	done
	echo "android-env: down"
}

case "${1:-}" in
up) up ;;
down) down ;;
run)
	shift
	up >/dev/null
	# bionic does not search /system/lib64 for /vendor binaries by default
	LD_LIBRARY_PATH=/system/lib64 exec chroot "$ROOT" "$@"
	;;
*) echo "usage: $0 up|down|run CMD..." >&2; exit 2 ;;
esac
