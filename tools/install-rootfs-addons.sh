#!/bin/sh
# Install rootfs-addons/ into a pmbootstrap-built greatlte disk image
# (partition 2 = pmOS_root) and enable the services, so the image boots to
# Plasma Mobile with GPU, WiFi and Bluetooth without any manual steps.
#
# usage: sudo tools/install-rootfs-addons.sh samsung-greatlte.img
set -eu

IMG=$1
ADDONS=$(cd "$(dirname "$0")/../rootfs-addons" && pwd)

LOOP=$(losetup -fP --show "$IMG")
MNT=$(mktemp -d)
trap 'umount "$MNT" 2>/dev/null; losetup -d "$LOOP"; rmdir "$MNT"' EXIT
mount "${LOOP}p2" "$MNT"

# GPU bring-up (real G3D PLL, 546 MHz) and CPU clocks after boot
install -m 755 "$ADDONS/etc-init.d/g3d" "$MNT/etc/init.d/g3d"
install -m 755 "$ADDONS/etc-local.d/cpuspeed.start" "$MNT/etc/local.d/cpuspeed.start"

# Bluetooth: UART attach, started/stopped from rfkill
install -m 755 "$ADDONS/etc-init.d/hciattach" "$MNT/etc/init.d/hciattach"
install -D -m 644 "$ADDONS/etc-udev-rules.d/10-hciattach.rules" \
	"$MNT/etc/udev/rules.d/10-hciattach.rules"

# No RTC: resync the clock once WiFi is up
install -D -m 755 "$ADDONS/etc-NetworkManager-dispatcher.d/50-chrony-resync" \
	"$MNT/etc/NetworkManager/dispatcher.d/50-chrony-resync"

# Discover's app catalogues (Alpine AppStream + Flathub) on first connection
install -D -m 755 "$ADDONS/etc-NetworkManager-dispatcher.d/60-appstream" \
	"$MNT/etc/NetworkManager/dispatcher.d/60-appstream"

install -D -m 755 "$ADDONS/usr-local-bin/bt-pan-up.sh" "$MNT/usr/local/bin/bt-pan-up.sh"

# BCM4361 WiFi (brcmfmac looks for both the generic and the board name)
FW="$MNT/lib/firmware/brcm"
mkdir -p "$FW"
for n in brcmfmac4361-pcie brcmfmac4361-pcie.samsung,greatlte; do
	install -m 644 "$ADDONS/firmware/bcmdhd_sta.bin_b0" "$FW/$n.bin"
	install -m 644 "$ADDONS/firmware/bcmdhd_clm.blob" "$FW/$n.clm_blob"
	install -m 644 "$ADDONS/firmware/nvram.txt_murata_r033_b0" "$FW/$n.txt"
done
# BCM4361 Bluetooth patchram
install -m 644 "$ADDONS/firmware/bcm4361B0_murata.hcd" "$ADDONS/firmware/bcm4361B0_semco.hcd" "$FW/"
# hciattach (bcm43xx) looks for the chip's ROM name in /etc/firmware; the
# greatlte module is the semco one
install -D -m 644 "$ADDONS/firmware/bcm4361B0_semco.hcd" "$MNT/etc/firmware/BCM4347B0.hcd"

# Audio: ABOX Calliope firmware (from the stock N950F vendor image) and the
# ALSA UCM profile (earpiece / headphones) PulseAudio uses for the card
install -m 644 "$ADDONS/firmware/calliope_sram.bin" "$ADDONS/firmware/calliope_dram.bin" \
	"$ADDONS/firmware/calliope_iva.bin" "$MNT/lib/firmware/"
mkdir -p "$MNT/usr/share/alsa/ucm2"
cp -a "$ADDONS/usr-share-alsa-ucm2/." "$MNT/usr/share/alsa/ucm2/"
# PulseAudio: start on the bottom speaker, not the last saved default
# Codec mic supply before the codec (see the file)
install -D -m 644 "$ADDONS/etc-modprobe.d/greatlte-audio.conf" \
	"$MNT/etc/modprobe.d/greatlte-audio.conf"
install -D -m 644 "$ADDONS/etc-pulse-default.pa.d/greatlte-speaker.pa" \
	"$MNT/etc/pulse/default.pa.d/greatlte-speaker.pa"
# ...and again after login: callaudiod makes the card's first sink (Earpiece) default
install -D -m 755 "$ADDONS/usr-libexec/greatlte-speaker-default" "$MNT/usr/libexec/greatlte-speaker-default"
install -D -m 644 "$ADDONS/etc-xdg-autostart/greatlte-speaker-default.desktop" \
	"$MNT/etc/xdg/autostart/greatlte-speaker-default.desktop"

# Charging: USB PD 9 V, per-source current limits, battery temperature
install -m 755 "$ADDONS/etc-init.d/greatlte-charging" "$MNT/etc/init.d/greatlte-charging"
install -D -m 755 "$ADDONS/usr-libexec/greatlte-charging" "$MNT/usr/libexec/greatlte-charging"
install -D -m 755 "$ADDONS/usr-local-bin/charger" "$MNT/usr/local/bin/charger"

# zram swap (lzo-rle) and the swap file behind it are sized from deviceinfo
for s in g3d hciattach local postmarketos-zram-swap swapfile greatlte-charging; do
	ln -sf "/etc/init.d/$s" "$MNT/etc/runlevels/default/$s"
done

# MTP (usb-mode mtp): files created as the phone user, not root
install -D -m 644 "$ADDONS/etc-umtprd/umtprd.conf" "$MNT/etc/umtprd/umtprd.conf"
install -D -m 755 "$ADDONS/usr-local-bin/usb-mode" "$MNT/usr/local/bin/usb-mode"

# Qt glyph cache workaround (under test: letters missing in Plasma labels)
install -D -m 644 "$ADDONS/etc-xdg-plasma-workspace-env/glyphcache-workaround.sh" \
	"$MNT/etc/xdg/plasma-workspace/env/glyphcache-workaround.sh"

# Modem (Shannon CP) bring-up: Samsung's cbd run from the phone's SYSTEM
# partition, and the IPC monitor. Installed but NOT enabled: see
# docs/RIL-BRINGUP.md for the test sequence (back up EFS first).
install -m 755 "$ADDONS/etc-init.d/cbd" "$ADDONS/etc-init.d/modem-ipc" "$MNT/etc/init.d/"
install -D -m 755 "$ADDONS/../tools/modem/android-env.sh" "$MNT/usr/local/bin/android-env"
install -D -m 755 "$ADDONS/../tools/modem/rmnet-up.sh" "$MNT/usr/local/bin/rmnet-up"
install -D -m 644 "$ADDONS/etc-udev-rules.d/61-greatlte-modem.rules" \
	"$MNT/etc/udev/rules.d/61-greatlte-modem.rules"
install -D -m 644 "$ADDONS/etc-udev-rules.d/78-greatlte-mm.rules" \
	"$MNT/etc/udev/rules.d/78-greatlte-mm.rules"
# ModemManager is D-Bus-activated as soon as Plasma asks for it, which can
# beat the OpenRC service: start it with the same options either way
install -D -m 644 \
	"$ADDONS/usr-local-share-dbus-1-system-services/org.freedesktop.ModemManager1.service" \
	"$MNT/usr/local/share/dbus-1/system-services/org.freedesktop.ModemManager1.service"
# Call audio (earpiece and mics while a call is up); enable it together
# with the modem: rc-update add callaudio default
TOOLS="$ADDONS/../tools"
install -D -m 755 "$TOOLS/callaudio/callaudio" "$MNT/usr/local/sbin/callaudio"
install -m 755 "$TOOLS/callaudio/callaudio.initd" "$MNT/etc/init.d/callaudio"
install -D -m 644 "$TOOLS/callaudio/callaudio.confd" "$MNT/etc/conf.d/callaudio"

echo "addons installed into $IMG"
