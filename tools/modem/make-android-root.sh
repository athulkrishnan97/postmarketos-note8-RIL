#!/bin/sh
# Build the minimal Android root that Samsung's cbd needs, from TWRP.
#
# The phone's SYSTEM partition may be empty (it is on the dev phone), but
# the TWRP image in RECOVERY carries Android 9 bionic: linker64, libc, libm,
# libdl, liblog, libcutils, libc++, libbase, libz. All symbols the LineageOS
# 18.1 cbd imports resolve against them (checked 2026-10-10). ld-android.so
# is provided by the linker itself.
#
# usage: make-android-root.sh RECOVERY.img cbd OUT.tgz
#   RECOVERY.img  dd of the RECOVERY partition (TWRP)
#   cbd           vendor/bin/cbd, e.g. from github.com/exynos8895/
#                 android_vendor_samsung_universal8895-common (lineage-18.1)
# Install on the phone: tar -C /var/lib/android-modem -xzf OUT.tgz
set -eu
img=$1; cbd=$2; out=$3
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# boot image v1: kernel and ramdisk are page-aligned after the header
python3 -I - "$img" "$tmp/ramdisk" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'ANDROID!', 'not an Android boot image'
ksz, _, rsz = struct.unpack_from('<3I', d, 8)
page = struct.unpack_from('<I', d, 36)[0]
ro = page + (ksz + page - 1) // page * page
open(sys.argv[2], 'wb').write(d[ro:ro + rsz])
PY

mkdir "$tmp/rd"
case $(head -c4 "$tmp/ramdisk" | od -An -tx1 | tr -d ' ') in
1f8b*)   gzip -dc "$tmp/ramdisk" ;;
5d0000*) xz --format=lzma -dc "$tmp/ramdisk" ;;
fd377a*) xz -dc "$tmp/ramdisk" ;;
*)       cat "$tmp/ramdisk" ;;
esac | (cd "$tmp/rd" && cpio -id 2>/dev/null)

r="$tmp/root"
mkdir -p "$r/system/bin" "$r/system/lib64" "$r/vendor/bin"
install -m 755 "$tmp/rd/sbin/linker64" "$r/system/bin/linker64"
for l in libc libm libdl liblog libcutils libc++ libbase libz; do
	install -m 644 "$tmp/rd/sbin/$l.so" "$r/system/lib64/$l.so"
done
install -m 755 "$cbd" "$r/vendor/bin/cbd"
tar -C "$r" -czf "$out" .
echo "android root: $out"
