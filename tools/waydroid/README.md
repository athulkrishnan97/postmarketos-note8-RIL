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
5. `rc-service waydroid-container start` (or add it to the default
   runlevel), then start "Waydroid" from the launcher or
   `waydroid show-full-ui`.

Known: the Android clock widget's digits render as blocks (Panfrost/Mesa
text rendering, like the GTK4 issue).
