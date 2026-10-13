# Waydroid on the Galaxy Note 8

LineageOS 20 (Android 13) in a Waydroid container, rendering on Panfrost
(Mali-G71). Needs kernel r20 or later: binder/binderfs, DMA-BUF heaps and the
Android networking options (policy routing, the netd iptables matches and
targets, eBPF). Changing those options changes core structures (struct sock,
struct nf_conn), so the modules have to come from the same build as the kernel.

Setup:

1. `apk add waydroid dnsmasq`
2. Images: download the `waydroid_arm64` system (VANILLA) and vendor
   (MAINLINE) zips (https://ota.waydro.id/system/lineage/waydroid_arm64/VANILLA.json,
   https://ota.waydro.id/vendor/waydroid_arm64/MAINLINE.json give the URLs and
   sha256 ids), unzip system.img and vendor.img into /etc/waydroid-extra/images,
   then `waydroid init` (uses them instead of downloading).
3. Android's low memory killer: the image runs lmkd with fixed minfree levels,
   measured against the whole phone's memory, and kills the launcher and
   services right after boot although 2 GB are available. Use the PSI mode:
   add `ro.lmk.use_minfree_levels = false` under `[properties]` in
   /var/lib/waydroid/waydroid.cfg, then `waydroid upgrade --offline`.
4. Networking: rootfs-addons/etc-nftables.d/50_waydroid.nft (postmarketOS's
   default-drop firewall otherwise drops the container's DHCP/DNS and its
   forwarded traffic, although Waydroid's own lxc table accepts them) and
   rootfs-addons/etc-NetworkManager-conf.d/waydroid.conf (NetworkManager must
   not manage waydroid0/veth*). After reloading nftables restart
   waydroid-container: the reload drops Waydroid's NAT table.
5. On-demand start: rootfs-addons/usr-local-bin/waydroid (first in PATH)
   starts waydroid-container before UI/app/session commands, through
   rootfs-addons/etc-sudoers.d/waydroid-container (wheel may run exactly
   `rc-service waydroid-container start` without a password). The
   container is not in any runlevel; tapping "Waydroid" or an Android app
   in the launcher brings it up (~20 s to the Android UI).
6. Deadlock fix (Android restarts by itself, "Waydroid crashed"): the
   vendor image's hwcomposer.waydroid.so holds its windowsMutex in
   keyboard_handle_enter while calling IWaydroidTask::setFocusedTask, which
   needs system_server's WindowManager lock; system_server meanwhile holds
   that lock and calls IWaydroidWindow::setIdleInhibit, which needs
   windowsMutex. After 60 s Android's watchdog kills system_server. Hit when
   the Waydroid window gets focus while an app toggles keep-screen-on
   (video). hwc-focus-deadlock.py patches the binary to unlock before the
   call (refuses any other build, by sha256):
   ```
   cp /var/lib/waydroid/rootfs/vendor/lib64/hw/hwcomposer.waydroid.so .   # while running
   ./hwc-focus-deadlock.py hwcomposer.waydroid.so hwcomposer.patched.so
   sudo install -D -m 644 hwcomposer.patched.so \
       /var/lib/waydroid/overlay/vendor/lib64/hw/hwcomposer.waydroid.so
   ```
   then restart Waydroid. Remove the overlay file before moving to a new
   vendor image, or it replaces the new image's hwcomposer.

Known: the Android clock widget's digits render as blocks (Panfrost/Mesa
text rendering, like the GTK4 issue).
