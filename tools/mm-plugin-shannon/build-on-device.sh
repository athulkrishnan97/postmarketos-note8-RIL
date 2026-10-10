#!/bin/sh
# Build ModemManager's "shannon" plugin on the phone and install it next to
# the distribution's plugins. The plugin must match the installed
# ModemManager exactly (version and build options: suspend/resume support
# changes the class layouts), so it is built from the same release with the
# phone's own toolchain and headers.
#
#   sudo tools/mm-plugin-shannon/build-on-device.sh [workdir]
#
# Needs network access (apk, git) the first time.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=${1:-/var/tmp/mm-shannon}
tag=${MM_TAG:-1.25.95-dev}
plugindir=/usr/lib/ModemManager

ver=$(ModemManager --version | awk 'NR==1 {print $2}')
case "$tag" in
"$ver"*) ;;
*) echo "installed ModemManager is $ver, MM_TAG=$tag: set MM_TAG to match" >&2; exit 1 ;;
esac

apk add --virtual .mm-shannon-build build-base meson ninja git glib-dev \
	libgudev-dev elogind-dev libmbim-dev libqmi-dev libqrtr-glib-dev \
	dbus-dev gettext-dev libxslt linux-headers python3

mkdir -p "$work"
cd "$work"
[ -d ModemManager ] || git clone -q --depth 1 --branch "$tag" \
	https://gitlab.freedesktop.org/mobile-broadband/ModemManager.git
cd ModemManager
git apply --check "$here"/0001-*.patch 2>/dev/null && git apply "$here"/0001-*.patch

# Same feature set as the distribution build where it matters for the ABI
[ -d build ] || meson setup build \
	-Dsystemd_suspend_resume=true -Dsystemd_journal=false \
	-Dmbim=true -Dqmi=true -Dqrtr=true \
	-Dpolkit=permissive -Dintrospection=false -Dvapi=false \
	-Dbash_completion=false -Dsystemdsystemunitdir=no \
	-Dtests=false -Dexamples=false -Dman=false -Dgtk_doc=false
ninja -C build src/plugins/libmm-plugin-shannon.so

install -m 644 build/src/plugins/libmm-plugin-shannon.so "$plugindir/"
echo "installed $plugindir/libmm-plugin-shannon.so; restart ModemManager"
