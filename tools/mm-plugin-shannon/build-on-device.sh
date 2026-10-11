#!/bin/sh
# Build ModemManager's "shannon" plugin on the phone and install it next to
# the distribution's plugins. The plugin must match the installed
# ModemManager exactly (version and build options: suspend/resume support
# changes the class layouts), so it is built from the same commit with the
# phone's own toolchain and headers.
#
#   sudo tools/mm-plugin-shannon/build-on-device.sh [workdir]
#
# MM_COMMIT is the _commit of Alpine's modemmanager APKBUILD for the
# installed version (apk list -I modemmanager); the default matches
# 1.25.95_git20260709. Needs network access (apk, git) the first time.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=${1:-/var/tmp/mm-shannon}
commit=${MM_COMMIT:-d776ea38d29ca472a12323c1d45002ee19a66f57}
plugindir=/usr/lib/ModemManager

pkg=$(apk list -I modemmanager 2>/dev/null | awk '{print $1}')
case "$pkg:$commit" in
modemmanager-1.25.95_git20260709-*:d776ea38*) ;;
*) [ -n "${MM_COMMIT:-}" ] || {
	echo "installed $pkg: set MM_COMMIT to its Alpine _commit" >&2; exit 1; } ;;
esac

apk add --virtual .mm-shannon-build build-base meson ninja git glib-dev \
	libgudev-dev elogind-dev libmbim-dev libqmi-dev libqrtr-glib-dev \
	dbus-dev gettext-dev libxslt linux-headers python3 polkit-dev

mkdir -p "$work"
cd "$work"
if [ ! -d ModemManager ]; then
	git init -q ModemManager
	git -C ModemManager fetch -q --depth 1 \
		https://gitlab.freedesktop.org/mobile-broadband/ModemManager.git "$commit"
	git -C ModemManager checkout -q FETCH_HEAD
fi
cd ModemManager
git apply --check "$here"/0001-*.patch 2>/dev/null && git apply "$here"/0001-*.patch

# Same feature set as the distribution build where it matters for the ABI
if [ ! -f build/build.ninja ]; then
	rm -rf build
	meson setup build \
		-Dsystemd_suspend_resume=true -Dsystemd_journal=false \
		-Dmbim=true -Dqmi=true -Dqrtr=true \
		-Dpolkit=permissive -Dintrospection=false -Dvapi=false \
		-Dbash_completion=false -Dsystemdsystemunitdir=no \
		-Dtests=false -Dexamples=false -Dman=false -Dgtk_doc=false
fi
ninja -C build src/plugins/libmm-plugin-shannon.so

install -m 644 build/src/plugins/libmm-plugin-shannon.so "$plugindir/"
echo "installed $plugindir/libmm-plugin-shannon.so; restart ModemManager"
