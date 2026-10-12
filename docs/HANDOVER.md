# Samsung Galaxy Note 8 (greatlte, SM-N950F, Exynos 8895) — postmarketOS Native Display Bring-up
# Agent handoff summary — written 2026-10-05 after ~30 kernel iterations
# Phone runs postmarketOS from SD card; kernel flashed to BOOT partition via heimdall.

## GOAL
Native mainline display: DECON_f → DSI (DSIM0) → S6E3HA6 AMOLED panel, with VESA DSC
(2 encoders, 2 slices of 720×40, 8bpp). End goal: usable desktop (Plasma) on the panel.

## WHAT WORKS (verified)
- Boot to postmarketOS userspace, SSH over Wi-Fi at 192.168.1.175 (sometimes .187), ~2 min after flash.
- S2DOS03 panel PMIC (hsi2c@10860000/i2c@10000, addr 0x60): vdd_ddi_1p8/3p0/1p6 rails.
- DSIM0 host probe + S6E3HA6 panel probe: panel ID 817143 (a2 variant) reads reliably.
- DECON DRM/KMS driver registers: /dev/dri/card0, connector DSI-1, mode 1440x2960@60.
  card1 = panfrost (Mali-G71, GL ES 3.1 works).
- Full atomic modeset from userspace runs cleanly (modetest, weston with pixman):
  bridge pre_enable → panel init → CRTC enable → DSIM DSC handoff → display-on →
  frame-done interrupts tick continuously (~60/s while compositing).
- IDMA fetches the real framebuffer: verified BASE_Y=0x98200000 in the DMA registers,
  DMA completes with no error flags, data path end-to-end confirmed (panel pattern
  changes with framebuffer content).
- Wi-Fi, SSH, bt-pan script, everything else from earlier bring-up stages.

## WHAT DOES NOT WORK (the current problem)
The panel shows structured noise instead of the image. Verified facts about the noise:
- Responds to framebuffer content (red bg → purple noise) but never legible.
- Slice structure visible: seam at the slice boundary (720px), line/band structure.
- DCS reads work (0x0A = 0x9C; booster on, BGR bit set, row/col exchange set).
- DCS writes complete (ret 0) but display-affecting commands (0x22 all-pixel-off,
  0x29 display-on, 0x11 sleep-out) have NO visual effect on the noise.
- DSIM BIST (0x94) also showed the same noise (may be inconclusive: BIST in command
  mode may not transmit without trigger).
- Full downstream DDI init (DSC enable 0x9D 0x01 + 128B PPS, DSU scaler 0xBA,
  CASET/PASET, omok latches, TE) applied in probe/prepare — noise persists.
- 0x09 0x01 "DSC PRA" packet experiment: pattern changed slightly (only experiment
  that visibly changed anything besides content).

## ROOT-CAUSE BUGS FOUND AND FIXED THIS SESSION (in order)
1. fbdev NULL plane->state crash: missing drm_mode_config_reset() → fbdev modeset
   oopsed in drm_atomic_helper_plane_duplicate_state. Fix: reset before register.
   (Diagnosed via pstore dmesg record — see "pstore" below.)
2. Hard freeze right after fbdev probe: fbcon takeover holds console_lock while the
   first commit runs inside it; any stall = silent freeze + userspace blocks on
   /dev/console. Worked around by CONFIG_DRM_FBDEV_EMULATION=n (fbdev/fbcon gone;
   console stays on ramoops/serial). Note: for a final product you'd re-enable
   fbdev AFTER the pipeline is proven, or use deferred takeover.
3. "nobody cared" IRQ storms: bootloader leaves DSIM+DECON running; stale pending
   interrupts at request_irq → kernel disables the line → RX-driven panel ID reads
   randomly failed (-110). Fix: mask+clear all interrupt sources before request_irq;
   handlers claim+clear pending even when !powered.
4. Panel probe failed (-110) whenever a stale DSIM IRQ fired early → flaky boots.
   Same fix.
5. THE BIG ONE: IDMA fetch registers must be written to the DMA bank 0x128B1000,
   NOT the DPP bank 0x12851000. Downstream writes IDMA_* via dma_write (0x128B1000);
   we wrote the DPP bank. The real fetch DMA kept the BOOTLOADER's config
   (BASE_Y=0xCC000000 = old LineageOS framebuffer) → panel showed frozen stale
   garbage forever. Symptom: 100% content-independent noise, immune to all commands.
   Fix: DT reg "idma" = <0x128b1000 0x1000> + program DPU_DMA_CH_MAP (G0→ch1).
   After this, noise became content-responsive (purple for red bg). See v99.
6. GEM PRIME export: get_sg_table added → dma-buf export works.
7. panfrost GPU import of scanout buffers: failed with swiotlb bounce ("swiotlb
   buffer is full, sz 17MB"). Cause: CMA placed at 0xf8000000 (3.87–4.0GB) and
   panfrost has a 32-bit DMA mask with NO IOMMU → buffer crossing 4GB unmappable.
   Fix: bootargs cma=128M@0x80000000-0xa0000000. (kmscube still crashes in
   gbm_bo_get_device — the EIO import path needs further work; weston+pixman is
   the reference client and works.)

## CURRENT KERNEL STATE (v101, branch display-bringup in /tmp/kernel-patchwork)
Key commits on top of base (b6a8f5cf5):
- DSIM host driver: drivers/gpu/drm/bridge/exynos8895-dsim.c (+ .h)
- DECON KMS driver: drivers/gpu/drm/bridge/exynos8895-decon.c + regs-exynos8895-decon.h
- Panel: drivers/gpu/drm/panel/panel-samsung-s6e3ha6.c
- DT: arch/arm64/boot/dts/exynos/exynos8895.dtsi (decon_f node, dsim0 node),
  exynos8895-greatlte.dts (bootargs), exynos8895-pinctrl.dtsi (TE pin gpb0-1)
Bootargs: "fbcon=font:TER16x32 fbcon=defer softlockup_panic=1 hung_task_panic=1
panic=5 cma=128M@0x80000000-0xa0000000"
Config notes: CONFIG_DRM_EXYNOS8895_DECON=y, CONFIG_DRM_FBDEV_EMULATION=n,
CONFIG_STRICT_DEVMEM=n, CONFIG_SOFTLOCKUP_DETECTOR=y, CONFIG_PSTORE_* + ramoops.

Verified-correct register configuration (read live during streaming, matches
downstream Samsung dpu source at /home/athul/postmarketos/reference/android_kernel_samsung_universal8895):
- DSIM: CONFIG=0x048801ff (CPRS_EN, 4 lanes, EOTP, cmd mode), RESOL=0x0b9001e0
  (2960|480), CPRS_CTRL=0xa (2 slices, multi-slice packet), SLICE01=0x02d002d0,
  THRESHOLD=480, NUM_OF_TRANSFER=2960, PHY timings = downstream table row 890/900
  (PMS 5/691/2 = 898Mbps @ 26MHz ref).
- DECON: GLOBAL=0x133 running, DPC2=0x0b1 (DSCC→2 encoders→FF0/1→formatter→DSIM0),
  splitter/FF/formatter sized in compressed units (240 per encoder, 480 merged),
  DSC encoders at +0x4000/+0x5000: CONTROL0=0xf022a (DCG all, BYTE_SWAP, flatness 2,
  slice_mode_ch 1, CG_EN; dual-slice off), PPS = panel table except
  initial_dec_delay=0x01B4 hardcoded (downstream non-VESA build).
- PPS table (both DDI and encoder): 8bpc, 8.0bpp, 1440x2960, slice 720x40,
  chunk 720, xmit_delay 0x200, dec_delay 0x268 (DDI table) / 0x1B4 (encoder),
  RC model 8192, final_offset 0x10F0. Encoder pic_width=720 (half raster).
- Panel PPS table (s6e3ha6_great_a2_s3_panel.h "DSU MODE 1") matches the computed
  downstream dsc_calc_pps_info values exactly (except dec_delay).
- RAM layout: banks at 0x80000000, 0xc0000000 (1GB each), 0x880000000 (4GB).
  CMA must stay under 4GB (GPU has 32-bit mask, no SMMU in DT).

## DOWNSTREAM REFERENCE (read this code!)
/home/athul/postmarketos/reference/android_kernel_samsung_universal8895
- DPU: drivers/video/fbdev/exynos/dpu/ (decon_reg.c, dsim_reg.c, dpp_reg.c, regs-*.h)
- Panel: drivers/video/fbdev/exynos/panel/s6e3ha6/s6e3ha6_great_a2_s3_panel.h
  (init_cmdtbl order, PPS tables, keys: KEY1=0x9F A5A5/5A5A, KEY2=0xF0, KEY3=0xFC)
- Panel DT data: exynos8895-display-lcd.dtsi (s6e3ha6_great_ddi: hs-clk 898,
  pms 5/691/2, dsc_cnt 2, slice 2, slice_h 40)
- Mainline reference tree: /home/athul/postmarketos/reference/mainline and kernel-7.0-rc1

## DEBUGGING SCAFFOLDING (all in /home/athul/postmarketos/)
- work/reboot-download: static helper; `sudo /tmp/reboot-download` from the phone
  reboots into download mode. MUST re-copy to /tmp after every boot (/tmp is wiped).
- work/devmem: static busybox-style devmem. USAGE: `/tmp/devmem <addr>` READS.
  `/tmp/devmem <addr> <value>` WRITES. (Earlier sessions accidentally CORRUPTED
  CMU registers by treating the width arg as value — beware.) Key addresses:
  DECON 0x12860000, DSIM 0x12870000, DPU0 CMU 0x12800000, IDMA DMA bank 0x128B1000,
  DPU_DMA common 0x128B0000 (CH_MAP +0x04, GLB_CGEN +0x14, GLB_CONTROL +0x40),
  PMU syscon 0x16480000 (DPU0 cfg/status +0x4060/+0x4064, DPU1 +0x4080/+0x4084),
  DSC encoders 0x12864000/0x12865000.
- work/inspect-disp.sh: register dump script (push devmem to /tmp first).
- work/boot-vNN.img: every kernel build (v82..v101). v99/v100/v101 = current era.
  Last known "good boot + working KMS" = v101 (this session).
- /tmp/paneldbg/paneldbg.c + paneldbg.ko: OUT-OF-TREE DEBUG MODULE. insmod on the
  phone, then via /sys/module/paneldbg/parameters/:
    dcsread  "<cmd_hex>,<len>"   e.g. echo 0a,1 > .../dcsread   (DCS read)
    dcswrite "<cmd_hex>,<p1>,<p2>,..."  up to 32 params         (DCS write)
    pps      1|0   (send 0x9D 0x01/0x00 + full 128B PPS table)
    rawtx    "<dsi_type_hex>,<d0>,<d1>"  raw DSI short packet (e.g. 09,01)
  Built: cd /tmp/paneldbg && make -C /tmp/kernel-patchwork ARCH=arm64 LLVM=1
         CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules
- pstore/ramoops: /sys/fs/pstore in TWRP (adb shell). pmsg-ramoops-0 = initramfs
  kernel log (circular, 16K). dmesg-ramoops-0.enc.z = panic/oops dump; decompress:
  python3 -c "import zlib;open('out','wb').write(zlib.decompress(open('f','rb').read(),-15))"
  NOTE: no dmesg record is written on a clean boot or key-reset — only panics
  (softlockup_panic=1 in bootargs makes lockups self-dump).
- Phone side: kmscube, modetest, weston (pixman), gdb, strace installed via apk.
  weston run recipe:
    sudo sh -c 'mkdir -p /run/xdg; chmod 777 /run/xdg;
      printf "[core]\nrenderer=pixman\n[shell]\nbackground-color=0xffRRGGBB\nidle-time=0\n" > /run/xdg/weston.ini;
      XDG_RUNTIME_DIR=/run/xdg nohup weston --config=/run/xdg/weston.ini \
        --backend=drm-backend.so --continue-without-input >/tmp/weston.log 2>&1 &'
  (idle-time=0 is REQUIRED — weston DPMS-off idles the CRTC otherwise.)
- SD rootfs writable from laptop at /run/media/athul/pmOS_root (user mounts it;
  TWRP cannot mount ext4). bootdiag openrc service installed: writes
  /var/log/bootdiag.log and copies to /boot/bootdiag.log (FAT, readable in TWRP).

## WORKFLOWS
Build+flash (fully authorized — do not ask before flashing when in download mode):
  cd /tmp/kernel-patchwork && make ARCH=arm64 LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j8 Image dtbs
  cd /home/athul/postmarketos/uniLoader && cp .../Image blob/Image && cp .../exynos8895-greatlte.dtb blob/dtb
  make -s ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- -j8
  cp uniLoader /home/athul/postmarketos/work/ulpkg/boot/bootshim
  mkbootimg --header_version 1 --kernel work/ulpkg/boot/bootshim --ramdisk work/empty_ramdisk \
    --pagesize 2048 --base 0 --kernel_offset 0x10008000 --ramdisk_offset 0x11000000 \
    --second_offset 0x10f00000 --tags_offset 0x10000100 -o work/boot-vNN.img
  heimdall flash --BOOT work/boot-vNN.img   (phone in download mode: VolDown+Home+Power)
Reboot to download from SSH: copy work/reboot-download to phone /tmp, sudo run it.
Phone sudo password: 147147. User puts phone into TWRP (VolUp+Home+Power) when asked;
adb works in TWRP (MTP mode on USB).

## FAILED EXPERIMENTS / DEAD ENDS (do not retry blindly)
- PPS before sleep-out at probe: wedges DSI command path (-110). Sleep-out must be
  the first command; full init runs in prepare().
- Sending init commands piecemeal mid-stream (paneldbg): no effect; init must be
  a sequenced whole before display-on (now implemented in v101).
- initial_dec_delay 0x0268 vs 0x01B4 in encoder: no visible change (weak knob).
- DSC encoder BYTE_SWAP off: pattern changed (whiter) but not fixed; restored ON
  (downstream has it ON).
- IDMA format ARGB8888 vs XRGB8888 live change: no visible change.
- DSIM MULI_SLICE_PACKET off: appeared to wedge (phone seemed stuck) — restored 0xa.
- modetest: sets mode then exits (tears down). kmscube: segfaults in
  gbm_bo_get_device (panfrost import EIO → swiotlb; CMA fix applied but kmscube
  still crashes — use weston+pixman as the test client).
- Writing 0x12800010 (DPU0 CMU) with devmem: wedged/rebooted the phone.
  The CMU is live-critical; read-only inspection is safe, writes are not.
- panfrost GPU import of display buffers: swiotlb bounce for >swiotlb segments;
  root cause = CMA placement vs 32-bit GPU mask. kmscube remains broken; if GPU
  rendering is needed, either place CMA low (done), enlarge swiotlb, or add the
  G3D SMMU to DT (exynos8895 has one; not enabled in mainline DT).

## LIVE OPEN HYPOTHESES (next things to try, in rough priority)
1. The panel likely still isn't in DSC decode mode: reads of 0x9D return 0x00
   (may be write-only, but combined with "0x09 0x01 changed the pattern", the
   Samsung-proprietary DSI packet type 0x09 ("DSC PRA" in their tree) may be the
   real enable path. Try: send rawtx 09,01 in prepare (in-kernel, proper sequence)
   BEFORE display-on. MAINLINE's MIPI_DSI_PICTURE_PARAMETER_SET=0x0a matches
   Samsung's DSC_PPS, but there is no mainline equivalent of their DSC_PRA=0x09.
2. Encoder reset-default PPS fields (bytes 36-57, 60-75: flatness qp, rc_model,
   rc_buf_thresh, RC range params 1-8) are unprogrammed on both ends — assumed
   HW defaults match. If the DSC core defaults differ from the panel table,
   decode fails exactly like this. Enumerate writable DSC_PPS registers
   (downstream regs-decon.h has DSC_PPS56_59 etc.) and program the full table
   4-87 on both encoders.
3. The DECON window data feeding the encoders may be in the wrong format/order:
   check blender/window BGR vs RGB (panel status bit says BGR; formatter RGB order
   currently DECON_RGB=0 — try BGR), and XRGB vs ARGB in the IDMA.
4. DSI link: reads work in LP; BTA during streaming fails (-74, probably normal).
   If 1-3 fail, scrutinize the HS clock-lane behavior (NONCONT_CLOCK_LANE set;
   TX_REQUEST_HSCLK reads 0 — verify against a downstream boot whether that bit
   is sticky; STATUS bit25 may be the HS-ready flag).
5. gamma/ELVSS/AOR are NOT sent (brightness-related only; would not fix noise
   but needed before the image is usable).

## KEY LESSONS
- pstore pmsg only covers initramfs; add softlockup_panic early (done) or you fly
  blind on freezes.
- /tmp on the phone is wiped every boot — re-push helpers.
- Always verify register writes with readback where the SFR allows; some Samsung
  SFRs read as 0 (write-only) — readback 0 is not proof of a failed write.
- The two-register-bank DPU (DPP vs DMA view) bit hard: 0x12851000 vs 0x128B1000.
- heimdall flash reboots the phone; after flashing, Wi-Fi/SSH takes ~2min.
- Deferred fbcon + no fbdev = no console on panel; screen will be black when the
  system is fine. Success criteria = SSH + interrupt counters + photos.

## UPDATE 2026-10-05 (Claude, v102-v104): PANEL SHOWS A CORRECT IMAGE
Root cause of the noise: DSC encoder SFR 0x58 holds PPS bytes 56..59. The driver
wrote only 0x0102 (bytes 58/59) with a full write, zeroing rc_buf_thresh[12..13]
(0x7d,0x7e) -> encoder RC model != panel decoder -> noise. Fixed in v104 (commit
cb5a79dbe). Found by booting with decon/dsim disabled in DT + clk_ignore_unused
(work/boot-v102-golden.img) and dumping the bootloader-programmed registers with
work/regdump (static /dev/mem range dumper: `regdump <hexbase> <hexlen> ...`).
Golden dump: work/regs-bootloader-golden.txt. Don't read DECON past +0x618 (bus error).
Also fixed: DSIM accepts type 0x07 compression-mode packets (panel now uses that
instead of DCS 0x9D), PER_FRAME_READ_EN cleared, SLICE23 set, init_dec_delay
0x268, bootloader window 5 disabled. Patch backup: work/patches-display-bringup/.

## UPDATE 2026-10-05 later (Claude, v105-v107): PLASMA MOBILE + TOUCH WORK
- v105: PRIME export used virt_to_page() on a WC remap -> bogus phys -> swiotlb; now
  dma_get_sgtable(). DRM driver name is "exynos" so Mesa kmsro pairs it with panfrost
  (kmscube 60fps on GPU). NOTE: kmscube quits instantly over ssh (stdin EOF) - run
  `sleep 30 | kmscube`.
- v106: touch is a Samsung Y661 (s6sy761 protocol); mainline driver failed because
  the first queued event wasn't boot-complete. Now polls like the vendor driver.
- v107: CMA 384M@0x80000000-0xbc000000 (KWin exhausted 128M).
- rootfs: 7 files in /etc/xdg were 0 bytes (incl. autostart/org.kde.plasmashell.desktop)
  -> plasmashell never started. Restored from *.apk-new. Likely from hard resets;
  fsck the SD rootfs from the laptop when convenient.
- tinydm enabled in default runlevel (autologin Plasma Mobile).
- OPEN: spontaneous reboots (2 seen); pstore stays empty (even console record) so
  the reset wipes RAM. Live log capture loop -> work/live-dmesg.log.

## UPDATE 2026-10-05 night (Claude, v108-v109)
- GPU ran at 26 MHz (oscillator): panfrost fdinfo drm-cycles / drm-engine ns = 26.0.
  Real G3D PLL is at CMU_G3D+0x140 (pmucal), not 0x120 (cmucal map; reads 0).
  v109: PLL at 0x140 recalc-only, busd mux no SET_RATE_PARENT, OPP 260 MHz,
  /etc/init.d/g3d runs steps 1 2 3 6 5. GPU now 260 MHz, Plasma ~16 fps.
- v108 (PLL with set_rate + 546 MHz OPP) FROZE at boot - do not repeat. Raising the
  clock must be done by an explicit, live-tested PLL reprogram with the GPU idle.
- work/reboot-download had NO sync() -> every reboot-to-download lost recent writes
  (cause of the 0-byte files). Fixed (sync added); always `sync` after rootfs edits.
- Plasma scale set to 3 (kscreen-doctor output.DSI-1.scale.3).
- Brightness: panel has no DCS 0x51; AOR (B1 hi lo, F7 03 latch, key2 F0 5A5A)
  dims, but raw writes mid-frame caused artifacts -> needs frame-synced backlight.
- v110: g3d step 7 relocks the G3D PLL to 546 MHz (service order 1 2 3 6 7 5), OPP 546.
- v110/v111: real vblank (trigger always armed, events from frame-start irq).
  Panel refresh measured 59 fps (DECON FRAME_ID @0x128602a0); Plasma ~24-30 fps,
  limited by plasmashell(~4ms)+kwin(~7ms) GPU latency per frame at 546 MHz.
- Reminder: /tmp on the phone is wiped every boot; re-push reboot-download/devmem.

## UPDATE 2026-10-06 (Claude): ROOTFS NOW ON INTERNAL UFS (v122)
- UFS driver: ufs-exynos 8895 variant (downstream table interpreter), HS-G3 x2, ~600 MB/s.
  Keys: disable HW ACG (HCI 0xFC), skip the secure UFS protector (FMP SMC hangs),
  NOT dma-coherent, UPMCRS read from UniPro 0x78EC (new get_upmcrs vop), FSYS0 bus
  clocks (AHBBR/RSTNSYNC/PMU/BTM, NOT the XIUs) on the UFS node, ufs-vcc always-on.
- Boot: pmos_boot_uuid=202ae7c7-... (sda16 CACHE, ext2 4K blocks!) and
  pmos_root_uuid=f9cb90c2-... (sda21 USERDATA, ext4) in DTB bootargs. UFS has 4K
  logical sectors: 1K-block filesystems will not mount.
- Android userdata/cache wiped (user approved). SD card no longer needed; to boot the SD
  again flash a kernel without the pmos_*_uuid args (e.g. work/boot-v121.img).

## UPDATE 2026-10-07 (Claude): BROWSER START-UP FREEZE — INVESTIGATION LOG (UNSOLVED)
Symptom: opening Firefox or Angelfish hard-freezes the phone within seconds of
the window appearing, BEFORE any page loads (user confirmed). Screen frozen,
WiFi/ssh gone, kernel log just stops: no panic, no soft/hard-lockup report
(HARDLOCKUP_DETECTOR_BUDDY is on), ramoops/console-ramoops empty after the
forced reboot (the reset does not keep RAM). Force reboot recovers.

Tests, in order (phone running v122 from UFS unless noted):
| # | Test | Result |
|---|------|--------|
| 1 | Rootfs moved from microSD to UFS | Still freezes -> NOT the SD/UHS bug |
| 2 | GPU at 260 MHz (dropped g3d step 7) | Still freezes -> not the GPU clock |
| 3 | `~/bin/cpuburn.sh 90`: 8 `while :` loops, 90 s, all cores at max (governor = performance) | OK |
| 4 | `~/bin/memburn.sh 90`: 8x `dd bs=32M` /dev/zero->/dev/null, 90 s | OK |
| 5 | glmark2-es2-wayland --off-screen (full run) | OK, no faults |
| 6 | glmark2-es2-wayland --fullscreen (full run, score 37) | OK; one harmless page fault in glmark itself |
| 7 | Angelfish with QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu | Froze |
| 8 | `~/bin/nsburn.sh 300`: 300x `unshare -Urn true` (browser sandbox namespaces; last dmesg lines before freezes are `lo` device_add) | OK in 5 s |
| 9 | `openssl speed -multi 8` AES-256-GCM + SHA-256 together (SIMD/crypto, load ~10) | OK |
| 10 | `~/bin/freezelog.sh`: per-second synced log of meminfo/top/temps + dmesg -w to ~/freezelog | Freeze ~3 s after Firefox start; 1.2 GB free -> not OOM; nothing in dmesg |
| 11 | `~/bin/sysprobe.sh`: read every file under /sys/devices, /sys/firmware, /sys/kernel (9482) one by one as user, path fsync'd first | OK |
| 12 | Firefox with LIBGL_ALWAYS_SOFTWARE=1 MOZ_WEBRENDER_SOFTWARE=1 (no panfrost at all; ~/.local/share/applications/firefox.desktop override still installed) | Froze |
| 13 | drm_info (enumerates card0 exynos + card1/renderD128 panfrost) | OK |
| 14 | `firefox --headless --screenshot` | OK, exits in 5 s |
| 15 | On-screen keyboard (tap a text field in drawer search) | OK |
| 16 | Firefox on X11 via Xwayland (MOZ_ENABLE_WAYLAND=0, DISPLAY=:0) | Froze |
Also: no /dev/video* devices exist (V4L2 probing ruled out); the network is
not involved (freeze precedes any page load).

Conclusion so far: it follows the browser WINDOW on the desktop, independent
of GPU rendering, Wayland vs X11, storage, CPU/memory load.
Next steps (not yet done):
- Open other heavy non-browser apps (Konsole, Discover, Dolphin) - none tried yet.
- strace the browser start-up into a log that survives the freeze (fsync per
  line, or pmsg if RAM retention can be made to work, e.g. via a watchdog
  warm reset).
- Display path: an unclocked DECON/DSIM register access when a new big
  window/plane appears (kwin direct scan-out, cursor/overlay planes?).
- Angelfish on the GPU also hit WARN panfrost_gem.c:213 (madv != WILLNEED on
  a PRIME self-import) and GPU DELAYED_BUS_FAULT / JOB_BUS_FAULT before a freeze.

Side fixes made during the hunt:
- Clock was 1970 every boot (no RTC driver; chronyd starts before WiFi and
  never syncs) -> /etc/NetworkManager/dispatcher.d/50-chrony-resync.
- Firefox/Konsole installed; KDE app cache must be rebuilt in Plasma's locale
  (LANG=en_US.UTF-8 -> ksycoca6_en_*), not the ssh shell's.
- Idle screen-off/dim disabled in powerdevilrc: after Plasma blanks the
  screen, the panel does not come back (DPMS on restarts DECON + DSIM with
  TE running but the panel stays black). The DSIM bridge/panel have no
  disable/enable hooks - needs fixing (separate bug).
- g3d boot script restored to steps 1 2 3 6 7 5 (546 MHz).
- GitHub master 5c5fd27: exynos8895-ufs.patch (= kernel 99941b8ed..6b7f51ad2,
  verified byte-identical), config UFS=y, pkgrel 30, docs §16 UFS, §17 freeze.
  The UUID bootargs commit (4d8c263ea) is device-specific and NOT in the patch.

--------------------------------------------------------------------------------
UPDATE 2026-10-07 evening (Claude): BROWSER FREEZE SOLVED, KERNEL 7.0, RELEASE
--------------------------------------------------------------------------------
Root cause of the browser freeze: firmware-owned RAM was mapped as ordinary
RAM. The stock greatlte DT reserves carveouts (/memreserve/ 0xE0000000
0x1900000 = secure world, plus seclog/secure_camera/abox/modem/cp_log/gnss);
ours reserved none. Browsers allocate hundreds of MB at start-up, touch a
page there, and the bus locks up (no log at all). Fix = no-map reservations
(kernel commit e5b6e7cd4, aports greatlte-reserved-memory.patch). TIMA at
0xB1000000 is NOT reserved: it splits the 384M CMA window -> "cma: Failed to
reserve 384 MiB" -> black screen (KWin dumb-buffer alloc WARN).
Additional tests after the table above:
| 17 | kernel 7.0 final (v123) | still froze |
| 18 | panfrost.transparent_hugepage=0 (v124) | still froze |
| 19 | strace streamed over ssh to laptop | URL Classifier thread killed by SIGSEGV inside read() into fresh 2MB mmap |
| 20 | cat the same file, caches dropped | fine -> the page, not the file |
| 21 | dmesg -w streamed over ssh | nothing before the stop (bus lock-up) |
| 22 | all stock carveouts no-map (v125) | Firefox fine, but CMA failed -> black screen |
| 23 | same minus TIMA (v126) | Firefox + Plasma fine -> SOLVED |
Boot images: v123 = 7.0, v124 = +THP off, v125 = +all carveouts, v126 =
carveouts minus TIMA (CURRENTLY FLASHED, UFS by UUID). Modules for 7.0.0 are
hand-installed in /lib/modules/7.0.0 (scratch build); 7.0.0-rc1 still there.
Kernel tree /tmp/kernel-patchwork has e5b6e7cd4 (carveouts) and dc578b987
(pmos.force-partition-resize in bootargs). 7.0 scratch tree: scratchpad/linux-7.0.
Polkit: phone had polkit-noelogind-libs -> Plasma "not authorized to control
networking". Fixed on UFS: apk add polkit-elogind '!polkit-noelogind-libs'
(world pins it), and the D-Bus-activated old polkitd had to be killed.
GitHub: master 8034944 (kernel 7.0-r1, both patches, addons, BRINGUP §17/§18).
Release v2026.10.07: boot-greatlte-7.0.img (generic, label-based) +
pmos-greatlte-plasma-mobile.img.xz (OpenRC Plasma Mobile, user/123456) +
SHA256SUMS. Test-booted from microSD: Plasma, WiFi, BT, Firefox, rootfs grow OK.
The microSD now holds the release image (old SD install overwritten, approved).
pmbootstrap: config restored (ui=buffyboard, user=athul); pmaports
device/testing synced from the repo aports (old copies in work/pmaports-backup-20261007).
2026-10-07 late: WiFi ~2 min delay fixed (txcap_blob sysfs-fallback timeouts) with
sysctl.kernel.firmware_config.ignore_sysfs_fallback=1 -> boot-v127 (CURRENTLY
FLASHED, UFS). Package 7.0-r2 (greatlte-no-fw-fallback.patch), GitHub 51c9c53,
release boot.img replaced. SD card REMOVED from the phone: rely on UFS only.
The release boot.img (label-based) hangs without the card - expected.
Phone IP changed to 192.168.1.187 (DHCP).

PENDING REPO CHANGES (not pushed yet; to be done together with the user)
- [ ] Kernel config: CONFIG_USB_DUMMY_HCD=n (aports config-postmarketos-exynos8895.aarch64).
      dummy_hcd + the initramfs USB gadget created usb0 (gadget) and usb1
      (cdc_ether host side of the same loopback) - a fake Ethernet in NM and
      "dummy_hcd timer fired" log spam. Tested on the phone as boot-v128
      (v127 + DUMMY_HCD off, Image from scratchpad/linux-7.0): usb0/usb1 gone.
      Needs: pkgrel 3, rebuild, refresh release boot.img, BRINGUP/README note
      (README "USB to a PC" row mentions dummy_udc).
- [ ] Phone-only: removed unudhcpd.usb0 (boot) and usb-signaller (default)
      from the runlevels. Consider doing the same in tools/install-rootfs-addons.sh.
- Note: polkit/modemmanager show "failed" in rc-status because D-Bus starts
  them before OpenRC (cosmetic); iio-sensor-proxy fails (no IIO sensors).
- [ ] Big cluster OPPs 1807/1937/2002/2158/2314 MHz (exynos8895.dtsi cluster1_opp_table;
      ECT table v1 group0 worst bin + ~12.5 mV: 1043750/1075000/1087500/1131250/1137500 uV).
      Old ">1.7 GHz hangs" note was the cluster swap (§15.3). 30 s per step, 0 hash errors.
- [ ] TMU THERMAL (the 2.3 GHz "crash" was thermal runaway: die >150 C, no throttling;
      even 1703/1690 all-core hit 94 C in 30 s and kept rising):
      * drivers/thermal/samsung/exynos_tmu.c: SOC_ARCH_EXYNOS8895 variant (+161 lines):
        per-sensor two-point trim (85C point at bit 9, calib sel bit 23), vref/slope from
        TRIMINFO0/1 bits 18+, read = max(sensor0, sensorN-20) over "samsung,sensors",
        Exynos7 threshold/IRQ/emulation paths. HW trip (CONTROL bit 12) left off.
        Reference: exynos8895/android_kernel_samsung_universal8895 lineage-19.1
        drivers/thermal/samsung/exynos_tmu.c (saved in reference/u8895-tmu/).
      * DT: tmu_cpu @0x10080000 (SPI 451, sensors 0x3f), tmu_gpu @0x10084000 (SPI 452,
        sensors 0x1), clock gout_peris_busif_tmu_pclk ("tmu_apbif"), #cooling-cells on
        all 8 CPUs, thermal-zones: cpu-thermal trips 83/88/93/98 passive (each forcing a
        minimum cooling state - plain step_wise with one trip let it sit at ~100 C) +
        115 critical; gpu-thermal 115 critical.
      * config: CONFIG_EXYNOS_THERMAL=y
      Tested (v132): 2314 big-only 2 min -> peak 92, steady 86 C; all-core schedutil
      2 min -> steady 88 C, 0 errors. TMU pclk is otherwise gated by clk_disable_unused
      (Bus error on /dev/mem reads) - devmem 0x10012020 0x00300000 to read it by hand.
- [ ] Phone /etc/local.d/cpuspeed.start: schedutil on both clusters, no cap (was
      performance + 1703 cap; old copy cpuspeed.start.orig-1703). rootfs-addons copy too.
- [ ] Phone ~/bin/soak.sh, ~/bin/bigburn.sh: hash-checking load tests (could go to tools/).
Boot image history: v129 OPPs to 2314; v130 2314@1162.5mV (didn't help - thermal);
v131 TMU driver + single trip; v132 graded trips (CURRENTLY FLASHED, UFS, phone .175).

PLASMA SMOOTHNESS INVESTIGATION (2026-10-07 night, unsolved; ~20-28 fps scrolling)
Ruled out / measured:
- CPU: kwin <=40% (spikes ~98%), plasmashell mostly <=30% of one core while scrolling.
- Memory bandwidth (CPU side, ~/bin/membw): read/write ~15 GB/s all-core, copy 18 GB/s
  traffic - healthy, so MIF is not starved. DO NOT devmem CMU_MIF (0x16x00100):
  reading PLL_MIF hung the bus -> forced restart.
- GPU clocks: G3D PLL 0xa0a80411 = 546 MHz, busd from the PLL (clk_summary).
- GPU raw: glmark2 1440x2960 off-screen build 500 fps (2.1 Gpx/s); on-screen fullscreen
  745 fps (direct scanout). Old 40 fps log predates the GPU clock fix.
- panfrost fdinfo (profiling=1): fragment engine busy/queued 150-660% while
  scrolling, tiler <=40% -> GPU-side, fragment, but not raw fill rate.
- Plasma Mobile blurs (BlurredBackground/ThumbnailStrip MultiEffect, BlurEffect
  FastBlur r=42) patched out via qmldir "prefer" removal: no visible gain -> reverted
  (backup ~/backup-mobileshell-qml).
- PAN_MESA_DEBUG=perf: 1322 "AFBC write staging blit" flushes in 20 s (+11k Gallium
  flushes). PAN_MESA_DEBUG=noafbc: no change -> reverted.
- KWin: DECON has 1 plane and no FB modifiers ("drmModeAddFB2WithModifiers is not
  supported") -> everything composited into a linear buffer; DECON page-flip event
  already completes at frame start (not after the DSI transfer).
Next ideas: KWin frame-timing (render vs vblank misses, GPU timer queries on panfrost),
DECON modifier/extra-plane support, check KWin triple buffering. User wants to stay
on Plasma (no DE change for now). Session restarts: tinydm restart leaves the old
startplasmamobile tree alive - kill it by explicit PID (pgrep -f self-matches).

PLASMA 30 FPS CAP - SOLVED (2026-10-07 late night)
- KWin_LOG_PERFORMANCE_DATA CSV: KWin renders in ~4 ms but every flip landed 2 refreshes
  after its target (locked to 30 fps; 20 fps when GPU-bound).
- GPU load while scrolling was mostly KWin's SOFTWARE BRIGHTNESS ("allowSdrSoftwareBrightness",
  brightness 0.29: the panel has no backlight control, so KWin recolours every pixel).
  At 100% KWin GPU fragment dropped to ~9% and predicted render to ~7 ms.
  -> real fix = hardware brightness in the S6E3HA6 panel driver (AOR/ELVSS tables).
- DECON timing trace (debugfs decon_timing, commit/start/done/event with shadow-req
  state): the driver requested only platform_get_irq(pdev, 1) = SPI 144 FRAME DONE;
  the FRAME START line (SPI 143, "frame-start" in our DT) was never requested, so the
  frame-start status was only seen at frame done (~14.9 ms later, 1.8 ms before the
  next TE latch). KWin got the flip event at frame done, committed ~3 ms later, just
  after the next latch -> every frame waited an extra refresh.
- Fix (boot-v134, phone): request both "frame-start" and "frame-done" by name; vblank +
  flip event now at frame start. Commit->event 2.8 ms, 255/255 commits 1 refresh apart
  (60 fps). KWin safety-margin override no longer needed (removed).
PENDING REPO CHANGES (add to the list above):
- [ ] exynos8895-decon.c: request the frame-start IRQ (platform_get_irq_byname
      "frame-start"/"frame-done", enable/disable both). Keep or drop the debugfs
      decon_timing trace (debug-only; useful but not for upstream-style patches).
- [ ] Phone: ~/.config/plasma-workspace/env/kwinperf.sh (KWIN_LOG_PERFORMANCE_DATA) -
      remove when done measuring. KWin brightness currently 100% (software brightness).

2026-10-07 23:30: PENDING REPO CHANGES above are PUSHED (GitHub 9f4e7b1, kernel 7.0-r3:
exynos8895-decon-frame-start.patch, exynos8895-thermal.patch, exynos8895-cpu-2314.patch,
EXYNOS_THERMAL=y, USB_DUMMY_HCD off, schedutil cpuspeed.start, install script drops USB
services, tools/soak.sh, README, BRINGUP §19-20). pmbootstrap build of r3 verified.
/tmp/kernel-patchwork has the same three commits. Release images NOT refreshed yet
(user: later). Phone runs boot-v134 (= r3 + debugfs decon_timing trace).
Still on the phone: ~/.config/plasma-workspace/env/kwinperf.sh (KWin frame CSV logging).
NOTE: docs/BRINGUP.md lines ~35 and ~213 contain the phone user password (from earlier
commits, already public on GitHub).

2026-10-08: snapshot taken -> /home/athul/postmarketos/08-10-2026-Snapshot (boot-v134, UFS rootfs
img+tar, /boot img, restore README). Then 6 GB RAM fix:
- Stock DT has memory@900000000 with size 0 (bootloader fills it in); our uniLoader-booted DT
  never gets that fixup, so only 3.7 GB was visible.
- boot-v135 (CURRENTLY FLASHED) = r3 kernel (no trace) + <0x9 0x0 0x80000000> in the greatlte
  memory node -> MemTotal 5,770,860 kB; ~/bin/memfill 3520 MB x2 verify: 0 bad words.
PENDING REPO CHANGE: add <0x9 0x00000000 0x80000000> to memory@80000000 in the greatlte dts
(new small patch, pkgrel 4) + README "Full 6 GB RAM" row -> works.
2026-10-08: RAM fix PUSHED (GitHub 517e8d9, kernel 7.0-r4, greatlte-6gb-ram.patch; r4 build verified).
Discover installed on the phone (discover + apk + flatpak backends, flatpak, Flathub remote system-wide;
9 new packages, no upgrades). Phone runs boot-v135 (= r4 kernel).

USB DEVICE MODE - WORKING (2026-10-08, boot-v138 on the phone, NOT in the repo yet)
- PHY: new "samsung,exynos8895-usbdrd-phy" variant in phy-exynos5-usbdrd.c, port of the vendor
  CAL (phy-samsung-usb3-cal.c, version 01_1_1 "KC"): PMU 0x704 bit0 de-isolate, refclk select
  0x10e5007c bit24, registers start after a version word at +0 (0x10000 -> +4 shift), Q-channel
  WA (PHYRESUME + LINKSYSTEM soft reset), PHYCLKRST REFCLKSEL=CLKCORE FSEL=26 MHz, port 1
  (0x10e10000) powered down, UTMI VBUS forced (no VBUS pad), OTG off.
  Vendor source: exynos8895/android_kernel_samsung_universal8895 lineage-19.1
  drivers/phy/phy-exynos-usbdrd.c + phy-samsung-usb3-cal.c (saved in reference/u8895-usb/).
- DWC3 glue: "samsung,exynos8895-dwusb3" clocks aclk/susp/ref + bus_lhm/bus_us + USBTV XIU/AHB.
  KEY GOTCHA: gout_fsys0_usbtv_i_usbtvh_xiu_clk carries ALL register access to the DRD block
  and PHY; unclaimed it is gated by clk_disable_unused right after probe -> every register
  reads 0 and nothing reaches the cable. DWC3 core is 2.80a (GSNPSID 0x5533280a).
- DT: usbdrd @0x10c00000 (dwc3 child IRQ SPI 337, dr_mode peripheral, high-speed for now),
  usbdrd_phy @0x10e00000 size 0x50080.
- Config: CONFIG_USB_ETH off (legacy g_ether grabbed the UDC before pmOS's configfs gadget and
  its RNDIS link timed out). pmOS initramfs NCM gadget + unudhcpd on usb0 (172.16.42.1) work:
  ping 2.4 ms, SSH, 32 MB/s laptop->phone, 15 MB/s phone->laptop (ssh-encrypted).
- Plug orientation was a red herring (gpi1-7 = 0 in both cases); failures were a cable already
  attached at boot (needs one re-plug - still to fix) and the wedged g_ether.
- Laptop quirk: enp3s0 has a static 172.16.42.2/24 that wins the route; needed
  `sudo ip route add 172.16.42.1/32 dev enx...` for the test (temporary).
- Phone: unudhcpd.usb0 (boot) and usb-signaller (default) are back in the runlevels.
2026-10-08 ~10:50: USB committed and pushed (777caea) in postmarketos-note8 (kernel 7.0-r5,
exynos8895-usb.patch). Cable-at-boot fixed by max77865-muic (MUIC CONTROL1 0x19 = 0x89; it was 0 =
open). /tmp/kernel-patchwork has the same 4 commits. Phone runs boot-v140 (= r5 + phone dts).
DON'T unbind exynos-dwc3 at runtime (kernel crash, then reboot hang).
2026-10-08: ADB over USB - Alpine adbd is TCP-only (no FunctionFS), needs LD_LIBRARY_PATH=/usr/lib/art.
/etc/init.d/adbd (rootfs-addons) runs it as uid-10000 user on 5555 via supervise-daemon; firewall allows
usb* only. `adb connect 172.16.42.1:5555`. push/pull ~2 MB/s (scp ~30). Committed locally (c45d41a), not pushed.
2026-10-08: MTP: 'usb-mode mtp' (usb-signaller + umtprd, /etc/umtprd default_uid 10000). Local commit, not pushed.
2026-10-08: ADB removed again (user: SSH over USB is enough). adbd/art_standalone uninstalled, service dropped (commit 09d800d, local).
2026-10-08: PUSHED master 777caea (USB device mode r5 + MTP). Release image still old.
2026-10-08: v143 = battery fuel gauge (max17042, maxim,max77865-battery). Charger watchdog (0x69 CNFG_00 WDTEN) stops charging ~3 min after boot; no charger driver yet. Phone USB MAC is random per boot; laptop route to 172.16.42.1 goes via enp3s0 - use ping -I / link-local.
2026-10-08: v144 = fuel gauge + charger watchdog driver; charging ~1A on wall charger. Repo commit local only (r6), not pushed.
2026-10-08 13:15: PUSHED c1bc08e. Phone v148 = r7 + phone dts (no THP param). Glyph-cache workaround under test; DoubleTapWakeup=false in user kwinrc.

## UPDATE 2026-10-08 afternoon - 2026-10-09 (Claude)
- Audio (kernel r8, GitHub 82cb145): CS47L93 codec + ABOX (Calliope firmware CQK0 in
  /lib/firmware) via a new sound/soc/samsung/abox driver; earpiece through PulseAudio + UCM,
  capped at -6 dB. Lessons: SMC_CMD_REG hangs (write the ABOX GIC directly); exynos-iommu
  needs the master runtime-PM active or the SysMMU stays bypassed; unmapped IOVA = IRQ storm.
- Kernel r9 (7812449): panel brightness through AOR (0xb1; KWin dropped software
  brightness), capacity-dmips-mhz (M2 1024 / A53 572; CPU map 0,5-7 = A53, 1-4 = M2),
  ABOX VSS window mapped.
- Kernel r10 (2410937) + release v2026.10.09: bottom speaker (MAX98506 on UAIF4), ABOX DMA
  position fix (count is in 16-byte units). Release microSD image NOT boot-tested.
- 2dfc2b6: zram (lzo-rle, kernel has only LZO) + 10 GB /swapfile behind it
  (device-samsung-greatlte r3); PulseAudio default sink = speaker
  (rootfs-addons/etc-pulse-default.pa.d). Elisa 26.08 OOMs on 3x scale without swap
  (13824^2 placeholder image; fixed upstream in Elisa c5d1a5c).
- c33fa38, kernel r11: CPU voltages = stock for this chip's fused ASV bin + 25 mV.
  ASV fuses at 0x10009000 (ungate OTP clock: devmem 0x10012034 0x00300000, restore
  0x00100000): table 8, big group 6, little group 7, g3d group 4. The bare stock table
  (v179) froze in normal use after passing a hash test; +25 mV (v180) is current.
  CPU trips 95/100/110 C + 115 critical (user choice; stock starts at 83 C). GPU still
  546 MHz only, 812.5 mV (stock for this chip 662.5); stock GPU steps 260/338/385/455/546
  (v108 froze when the kernel drove the G3D PLL - test carefully).
- Stock thermal reference: big IPA from 83 C, big cores hot-unplugged at 96 C; little caps
  81 C -> 1456 ... 96 C -> 598 MHz; GPU 78/88 C; TMU hardware trip at 115 C
  (CONTROL bit 12 - still off on mainline).
- Phone: boot-v180 on UFS, SSH athul@172.16.42.1 (USB) or WiFi DHCP. bootdiag service
  disabled (it rewrote /boot every 30 s; a freeze left /boot needing fsck).
- Open: 5 GHz WiFi boot freeze, microphones, jack detection, speaker DSM protection,
  GPU DVFS, TMU hardware trip, per-chip ASV selection at boot (voltages are hard-coded
  for one chip), release images not refreshed since r10.

## UPDATE 2026-10-09 evening (Claude, cloud session, no phone access): MODEM / RIL START
Plan: docs/RIL-PLAN.md. Test sequence for the phone: docs/RIL-BRINGUP.md.
- Hardware (stock modem-ss355ap-pdata.dtsi): Shannon 355 CP on the SoC, shared memory
  in modem_if (0xF4C00000, 144 MiB: CP 122M | VSS 6M | IPC 5M @+0x8000000 | ZMB 11M),
  mailbox mcu_ipc@15B40000 SPI 97, CP IRQs SPI 21 (cp_fail) / 57 (cp_wdt), CP secure
  boot: CP_CTRL via SMC 0x82000700 (READ_CTRL 3 / WRITE_CTRL 4), image check via
  IOCTL_SECURITY_REQ -> SMC 0x82000700 mode/size_boot/size_main (SSS clock SMC 0x82001011).
- Kernel: Samsung modem_v1 + mcu_ipc + shm_ipc + pmu-cp ported to 7.0 as
  drivers/misc/modem_v1 (vendor code reuse approved by the user). Compat shims in
  include/mif_compat.h (wakelocks -> wakeup sources, exynos_smc -> arm_smccc_smc,
  exynos_pmu_read/write -> PMU regmap, PM QoS -> logged no-op). Vendor panics on CP
  sequencing timeouts / IOCTL_MODEM_CP_UPLOAD replaced by WARN/log. modem_if is no longer
  no-map (stock layout: driver uses phys_to_virt on the ZMB pool), still reserved.
  bufpool_2nd (stock 0xE9000000) set to 0: overlaps abox/video carveouts.
- Userspace: run Samsung's own cbd from the phone's SYSTEM partition in a chroot
  (tools/modem/android-env.sh, rootfs-addons/etc-init.d/cbd); tools/modem/mifmon.c holds
  umts_ipc0 + umts_rfs0 open (kernel sends INIT_END only then) and decodes IPC/RFS frames;
  -a sends AT on umts_router (decides ModemManager-over-AT vs a Samsung IPC daemon).
  smcprobe.ko is the first test (SMC go/no-go). RFS is a file protocol (RFS_OPEN_FILE,
  READ, WRITE, LSEEK, ...; libsec-ril class IpcProtocol41Rfs) - needs answering next.
- Reference blobs: github.com/exynos8895/android_vendor_samsung_universal8895-common
  (lineage-18.1): vendor/bin/cbd, lib64/libsec-ril.so. cbd args from init.baseband.rc:
  cbd -d -tss310 -bm -mm -P platform/11120000.ufs/by-name/RADIO -n /efs

## UPDATE 2026-10-10 (Claude, cloud session via CF tunnel -> laptop -> phone)
- Access: cloud session -> `cloudflared access ssh` to ssh.oakdalehome.uk (laptop athul-g15)
  -> ssh athul@172.16.42.1 (phone, USB). Up-to-date working tree on the laptop:
  /home/athul/postmarketos/ (postmarketos-note8 = original repo; work/linux-7.0 = kernel
  tree of the running #39 build). RIL work lives in github.com/athulkrishnan97/postmarketos-note8-ril.
- Merged original master (USB PD charging = kernel r12) -> modem kernel is now r13; the
  modem patch was regenerated on top of exynos8895-charging.patch and r13 builds.
- Backups: work/modem-backup-20261010/ on the laptop (EFS, CPEFS, RADIO, CP_DEBUG,
  PERSISTENT, PARAM, RECOVERY; SHA256SUMS verified). EFS has nv_data.bin (1 MiB) + .bak.
- RADIO TOC: BOOT @0x40000000, MAIN @+0x10000 (0x24adeb8 B), VSS @+0x7a00000, NV @+0x5900000
  (1 MiB) - matches the modem dtsi layout.
- SYSTEM partition is EMPTY (only lost+found): no Android userspace on the phone. TWRP
  (RECOVERY, Android 9 / SDK 28, LZMA ramdisk) sbin/ has linker64 + libc/libm/libdl/liblog/
  libcutils/libc++/libbase/libutils/libz; all 90 symbols the lineage-18.1 cbd imports
  resolve against them -> build the cbd chroot from TWRP instead of SYSTEM.
- Plain non-secure reads of PMU CP_CTRL_NS (0x16480030) take a synchronous external abort
  (oops in smcprobe init). Use the SMC path only (the driver already does under
  CP_SECURE_BOOT).
- SMC 0x82000700 READ_CTRL (smcprobe2) then froze the phone (no ping/ssh) - but the first,
  oopsed module was still stuck "Loading" at the time. Retest from a clean boot pending.
- 2026-10-10 ~10:00: SMC test from a CLEAN boot (smcprobe2, pmu=0, phone dmesg streamed to the
  laptop): SMC 0x82000700 READ_CTRL (CP_CTRL_NS) hard-froze the phone at once (no ping; not
  even the pr_info just before the call reached the stream). CONFIRMED: the CP control SMC
  hangs on this boot chain. Working vs hanging monitor calls so far:
    works: 0x82000501 SMC_ABOX_PREPARE
    hangs: 0x82000700 CP READ_CTRL, SMC_CMD_REG (-101, SFR access; ABOX GIC), UFS FMP SMCs
  Both hanging classes make the monitor touch SFRs (PMU / GIC) on our behalf; the plain NS
  read of PMU CP_CTRL_NS aborts. Ideas: caller CPU (test taskset -c 0), boot-time monitor
  setup the stock kernel does (exynos-el3_mon.c: SMC_CMD_SET_SEH_ADDRESS -123,
  PROTECT_KERNEL_TEXT -125, SET_DEBUG_MEM -120; exynos-cpumap SMC_CMD_CPUMAP -10), or
  starting the CP from uniLoader before Linux.
- 2026-10-10 10:11, smcprobe4 seh=1 step=1, taskset -c 0, kmsg streamed with a 1 s delay
  before each SMC: log ends with "running on CPU 0 (MPIDR 80000100)" then
  "SMC SET_SEH_ADDRESS ..." -> the phone froze INSIDE SMC -123. DEFINITIVE: the SMCs hang
  in the secure path itself, from CPU 0 as well, and even the setup call stock Android makes
  at boot. Only 0x82000501 (ABOX prepare) is known to return.
  Lessons: `dmesg -w` into a pipe is block-buffered (lost lines); `cat /dev/kmsg` is not, but
  printk wakes readers lazily, so sleep ~1 s after a message before a call that may hang.
  Next hypothesis: an EL2 hypervisor (Samsung uH/RKP) trapping SMCs (HCR_EL2.TSC) whose
  memory we hand to Linux - stock reserves TIMA 0xB1000000 (2 MiB), our CMA window
  0x80000000-0xbc000000 covers it. Check "CPU: All CPU(s) started at EL1/EL2" in dmesg.
- 2026-10-10 10:21: boot-v183-tima.img (laptop work/) = v182 + DTB only: TIMA 0xB1000000
  (2 MiB) reserved no-map, cma=256M@0xa0000000-0xb1000000 (lands at 0xA1000000; Plasma OK,
  ~52 MiB CMA used at idle). Size 40,298,496 (BOOT is 41,943,040). Flashed remotely
  (reboot-download + heimdall from the laptop). smcprobe4 seh=1 step=1 STILL froze inside
  SMC -123 -> TIMA/CMA overlap is NOT the cause. v182 remains the known-good image.
- 2026-10-10 10:31: USB (dwc3, irq 49) is handled on CPU 0 only, so a CPU-0-only hang would
  also kill the link. Re-ran smcprobe4 seh=1 step=1 pinned to CPU 3 (MPIDR 0x80000002, a big
  core): log ends at "SMC SET_SEH_ADDRESS ...", then nothing - no ping, no soft/hard lockup
  report from the other CPUs. The SMC stops the WHOLE SoC, from any core.
- Kinibi (Trustonic TEE) context: stock builds drivers/gud/gud-exynos8895_kinibi400; CPU_IDS
  for 8895 = {0x100..0x103, 0x0..0x3} (logical 0-3 = A53, 4-7 = M2 in stock numbering);
  the secure OS only runs when the NWd driver schedules it (nq/yield SMCs). Mainline never
  starts it. Candidate explanation: monitor services that synchronise with the SWd block
  forever. Freeze tests stopped here; further work is research (see RIL-PLAN / this file).
- 2026-10-10 10:33: smcprobe5 kinfo=1 step=1 (CPU 0): Kinibi MC_FC_INFO(0xFF000002, 0) - the
  first, read-only call of the stock MobiCore init - ALSO froze the SoC (log ends at
  "SMC MC_FC_INFO(MCI_VERSION) ..."). Anything that enters the secure world hangs; only PSCI
  and 0x82000501 return. Initialising Kinibi from Linux is blocked at its first call.
  Next: boot TWRP (Samsung-derived kernel with drivers/gud built in) and read its dmesg for
  MobiCore/t-base init, to tell "our boot chain breaks the SWd" from "SWd dead for everyone".
- 2026-10-10 10:37 TWRP (kernel 4.4.111 omni-9.0, booted by S-Boot directly): the SAME SMCs
  WORK there: "exynos_set_seh_address: set the seh address to el3 monitor well", Kinibi
  t-base-EXYNOS64-Android-400A-V012 starts, CoreSwap 0->1 ok. => the secure world is fine;
  our boot path breaks it. (`reboot recovery` from pmOS did not reach TWRP; user booted it.)
  S-Boot passes its owned regions only on the cmdline (TWRP /proc/cmdline): ess_setup=0x91200000,
  sec_avc_log 0x92202000, sec_tsp_log 0x92244000, sec_debug.base 0x92286000 (1M),
  auto_summary_log 0x92388000, tima_log 0xb1000000, kaslr_region 0x80001000; FDT /memreserve/
  only 0xE0000000. uniLoader CONFIG_PAYLOAD_ENTRY=0x90000000 put our kernel on top of them.
- boot-v184-sbootmem.img (laptop work/): v183 + uniLoader PAYLOAD_ENTRY=0x80200000 (kernel now
  0x80200000-0x8292ffff) + 0x91000000-0x92ffffff reserved no-map + ramoops moved to 0x9f000000
  (pstore registers there). Boots, Plasma OK. smcprobe4 seh=1 step=1 STILL froze in SMC -123.
  => not the S-Boot debug areas either. Next isolation: boot TWRP's 4.4 kernel THROUGH
  uniLoader (uniLoader vs mainline), or diff what mainline does early (GIC, PSCI, timers).

## 2026-10-10 ~10:50 ROOT CAUSE OF THE SMC HANG: clk_disable_unused / unused power domains
- Isolation: TWRP's 4.4 kernel booted THROUGH uniLoader (laptop work/boot-twrp-via-uniloader.img:
  TWRP Image @0x80080000, its LZMA ramdisk @0x84000000, the S-Boot FDT pulled from a real TWRP
  boot with /chosen initrd moved) -> "set the seh address to el3 monitor well", Kinibi starts.
  So uniLoader is fine; the mainline kernel breaks the monitor.
- boot-v185-clkignore.img = v184 + "clk_ignore_unused pd_ignore_unused" in bootargs: SMCs WORK:
    SET_SEH_ADDRESS (-123)          -> 0x0
    Kinibi MC_FC_INFO(0xFF000002,0) -> ret 0, state 0 (NOT_INITIALIZED), info 0x10006
    CP READ_CTRL CP_CTRL_NS         -> ret 0, 0x002016a0 (CP_PWRON=0: CP off)
    CP READ_CTRL CP_CTRL_S          -> ret 0, 0x00000000 (CP_START=0)
  The monitor touches a block whose clock / power domain Linux gated as "unused"; that access
  hangs the bus (same class as the TMU-pclk and PLL_MIF hangs). TODO: find the exact
  clock(s)/domain(s) and claim them in DT instead of the blanket ignore_unused.
- Layout changes carried in v184/v185 (keep): kernel @0x80200000 (uniLoader PAYLOAD_ENTRY),
  0x91000000-0x92ffffff no-map (S-Boot ess/sec_debug areas), ramoops @0x9f000000, TIMA
  0xb1000000 no-map, cma=256M@0xa0000000-0xb1000000.

## 2026-10-10 ~11:05 THE MODEM BOOTS ON MAINLINE AND ANSWERS AT
Phone: boot-v186-modem.img (laptop work/) = laptop tree work/linux-7.0 copy (work/linux-7.0-ril)
+ exynos8895-modem.patch, SEC_SIPC_MODEM_IF=y, phone DTB with v185 layout + clk_ignore_unused
pd_ignore_unused, uniLoader PAYLOAD_ENTRY 0x80200000. 40,441,856 bytes (BOOT 41,943,040).
- Driver probes: mcu_ipc, shm_ipc (ipc_off 128M, 5M), ss355ap ctl + shmem link, /dev/umts_boot0,
  ipc0/1, rfs0, csd, router, dm0, ramdump0, rmnet0-7.
- Android root for cbd = TWRP bionic (RECOVERY ramdisk sbin/: linker64 + libc libm libdl liblog
  libcutils libc++ libbase libz; ld-android.so is internal to the linker) + lineage-18.1 cbd,
  in /var/lib/android-modem (tools/modem/make-android-root.sh). Run with
  LD_LIBRARY_PATH=/system/lib64. cbd drops to uid 1001 (radio): /dev/umts_* and RADIO (sda9)
  must be 1001:1001 0660 (rootfs-addons/etc-udev-rules.d/61-greatlte-modem.rules).
- `cbd -d -tss310 -bm -mm -P platform/11120000.ufs/by-name/RADIO -n /efs` with mifmon holding
  ipc0+rfs0: TOC read (BOOT/MAIN/VSS/NV), SECURITY_REQ mode 2 then mode 0 (size 7860, 38461112)
  -> return 0, MODEM_ON, BOOT_ON (cp_status=1), DL_START (magic BOOT), BOOT_OFF: INIT_START ->
  SBD setup -> PIF_INIT_DONE -> CP_START -> INIT_END -> state ONLINE.
- CP sends FMT unsolicited: PWR 0x01, SEC 0x01/0x07 (SIM), GPRS 0x0f, CALL 0x16, NET 0x03/0x05,
  DISP 0x01/0x06; RFS requests start (unanswered).
- AT on /dev/umts_router WORKS: AT->OK, AT+CGMR/ATI -> N950FXXSGDUG6, AT+CPIN? -> SIM not
  inserted (ask user whether a SIM is in), AT+COPS? -> +COPS: 0, AT+CSQ -> 17,0.
  => Userspace route A: ModemManager over AT (umts_router), data via rmnet/multipdp.
- Harmless cbd warnings: NV validation TIMEOUT (Android property), /sys/power/wake_lock missing.
- Next: SIM check; RFS server (cbd/rild normally answer RFS - CP file requests to /efs);
  expose umts_router to ModemManager (WWAN AT port or a udev/ID_MM tag on a tty bridge);
  data path; find the exact clocks behind clk_ignore_unused.

## 2026-10-10 ~11:50 LTE REGISTRATION AND MOBILE DATA WORK
SIM (Lidl Connect IE) inserted, same v186 boot, cbd + mifmon running.
- After SIM: +CPIN READY, +COPS 0,0,"Lidl Connect",7 but +CEREG 0,2 (searching), CGATT 0,
  CGDCONT empty. The CP does not attach on its own: the stock RIL sets the initial-attach
  context. `AT+CGDCONT=1,"IPV4V6",""` + `AT+CGATT=1` -> +CEREG stat 1 (home, LTE), CGATT 1,
  CGACT 1,1. (Cell reselection between two cells shows brief stat 2 URCs; normal at CSQ 8-10.)
  CREG stays 0,0 (no CS domain on LTE-only; voice/SMS need IMS or CSFB - not looked at yet).
- An AT-activated context is NOT routed to rmnet: TX leaves the AP, nothing comes back
  (napi rx_poll_count per PS ring stays 0). AT+CGDATA only offers "IP"/"PPP" on the AT channel.
- Fix: Samsung IPC over umts_ipc0, decoded from the stock libsec-ril.so (N950FXXUGDZEC system
  image on the laptop, work/stock/libsec-ril.so; IpcProtocol41::IpcTxDefinePdpContext /
  IpcTxPsAttach / IpcTxSetPdpContext):
    0d 01 SET  DEFINE_PDP_CONTEXT  [1, cid, pdp_type(6=v4v6), apn[100], ...] 134 bytes
    0d 03 SET  PS attach           [1, 0, 0] 10 bytes (-> 0x8005 when already attached)
    0d 04 SET  PDP_CONTEXT         [1=activate/0=deact, cid, auth...] 112 bytes
  Each answered by GEN_PHONE_RES 80 01 [grp idx type code16] (0x8000 OK, 0x8001 refused,
  0x8003 unsupported). Activate without a DEFINE first -> 0x8001.
  Then the CP sends GPRS_IP_CONFIGURATION 0d 09: [cid, flags 0x07, ?, ip4, dns1, dns2, ...,
  u16 mtu @0x9f] (IP differs from the stale AT+CGPADDR value!) and GPRS_CALL_STATUS 0d 10
  [cid, 1=active]. Context cid N -> rmnet(N-1).
- RX frame types differ from TX: 1 INDI, 2 RESP, 3 NOTI (mifmon printed them as EXEC/GET/SET).
- `mifmon -d -s /usr/local/bin/rmnet-up` does the whole sequence; rmnet-up sets the IP/32,
  MTU 1430, a metric-700 default route, /run/rmnet-resolv.conf. Ping 1.1.1.1 over rmnet0
  30-50 ms; HTTP to 1.1.1.1/cdn-cgi/trace -> loc=IE.
- Only ONE process may read umts_ipc0: a second reader (e.g. `mifmon -f` next to the
  service) steals the CP's replies.
- Group 0x03 idx 0x02 (IpcTxSetDataCallEstablish) -> 0x8003 on this CP; not needed.
- Still open: RFS server (CP file requests, len field parsing in mifmon is wrong for big
  frames: reads are 64 KiB-chunked), attach without AT (send DEFINE before attach at boot -
  untested whether the CP then attaches by itself), ModemManager integration, SMS/voice (IMS),
  exact clocks behind clk_ignore_unused.

## 2026-10-10 ~12:15 RFS SERVER: AT NO LONGER STALLS, DATA FROM A COLD MODEM BOOT
- After two modem restarts AT on umts_router went silent (writes OK, no replies; the CP kept
  sending FMT). Cause: unanswered RFS requests. Right after each CP boot it sends RFS OPEN
  requests and two NV writes; the AT interface stalls until they are answered. (The first
  session worked by luck of timing.)
- RFS protocol decoded from the stock libsec-ril (Nv::ProcessRfsPacket/ProcessRfsMessage and
  the Nv::Process* handlers); table in the header comment of tools/modem/mifmon.c. Highlights:
  frame = u32 len (whole frame, reassembled over reads, max 512 KiB), u8 cmd, u8 id; replies
  echo cmd/id. 01/02 NV read/write on /efs/nv_data.bin (offsets as given); after each write
  nv_data.bin.md5 = hex MD5(nv_data.bin + "Samsung_Android_RIL") (verified on the device's
  own nv_data.bin.md5 and .nv_data.bak.md5). File ops on /efs/root/<path> (stock), Linux
  open flags, replies carry fd/ret + errno. Unknown commands get -1/ENOSYS.
- What the CP asks at boot: OPEN "\x01" and OPEN "CP_AUDIO_SLSI.bin" (both ENOENT; the stock
  system/etc has neither file either), CLOSE x2, NV_WRITE 101132 bytes @0x80000 and
  ~18.6 KB @0x0 (the CP syncing its NV; ~15 KB of nv_data.bin changed vs the backup -
  expected). Backup of the pre-RFS NV on the phone: /root/nv-backup-202610101207/
  (plus the full EFS image on the laptop, work/modem-backup-20261010).
- `mifmon -r` answers RFS (EFS root via -e, default /var/lib/android-modem/efs). With RFS
  answered, AT replies are immediate after a fresh CP boot.
- `mifmon -d` now does the whole data bring-up itself: AT+CGDCONT + AT+CGATT=1 (retried),
  then IPC define + activate, then the hook. IPC PS attach (0d03) dropped: on its own it
  does not attach. The sequence restarts on PWR_PHONE_PWR_UP (01 01 NOTI, sent at every CP
  boot - the misc devices give no HUP on a CP restart) and on a call drop.
- Tested from a cold CP start: `mifmon -r -d -s rmnet-up` started first, then cbd ->
  PWR_UP -> RFS NV writes answered -> CGDCONT OK -> CGATT OK (~5 s) -> 0d01/0d04 0x8000 ->
  IP config -> rmnet0 up -> ping OK. Services reordered accordingly (modem-ipc before cbd,
  -r by default).

## 2026-10-10 ~12:40 SERVICES TESTED ON A REAL BOOT: MOBILE DATA 28 s AFTER POWER-ON
- Installed on the phone: /etc/init.d/{modem-ipc,cbd}, /usr/local/bin/{android-env,rmnet-up,
  mifmon}, /etc/udev/rules.d/61-greatlte-modem.rules, /etc/conf.d/modem-ipc (MIFMON_DATA=yes);
  both services in the default runlevel.
- First reboot: cbd could not open RADIO ("BIN ... open fail"): the old 60-greatlte rule
  matched ENV{ID_PART_ENTRY_NAME}, which 60-persistent-storage.rules sets only after it
  (lexical order). Now 61-greatlte-modem.rules matching the kernel's ENV{PARTNAME}.
- Second reboot, nothing touched: CP online ~20 s, RFS NV writes answered at 21.8 s, data
  call up at 27.6 s (default route via rmnet0 at metric 700), ping OK.

## 2026-10-10 ~13:25 MODEMMANAGER SEES THE MODEM (WWAN AT PORT PROXIED BY MIFMON)
Phone: boot-v188-wwan.img (laptop work/) = v186 + exynos8895-modem-wwan.patch (kernel tree
work/linux-7.0-ril; packed with work/ul-r13 + work/v186-inputs/dtb, saved there from /tmp).
v187 created the port from modem_probe -> "class_find_device called for class 'wwan' before
it was registered", no /dev/wwan0at0 (drivers/misc probes before drivers/net/wwan); v188
creates it from a late_initcall.
- Why a proxy: the attach must go over AT. IPC-only tests on a fresh CP (RFS answered):
  0d01+0d03 (PS attach) -> 0x8000 but never attaches and the AT engine wedges; 0d01+0d1b
  (data profile "lte_internet") -> same wedge. So any IPC GPRS setup before the AT attach
  wedges AT (this was also the earlier "AT stall" together with the missing RFS answers).
  `mifmon -I` (IPC attach) kept for experiments only.
- Kernel: wwan_at_bridge.c: WWAN AT port wwan0at0 (parent mif_pdata) <-> misc
  /dev/wwan_at_ctl. mifmon -M serves it: passes ModemManager's commands to umts_router,
  runs its own CGDCONT/CGATT only between them, forwards "+..." URCs that arrive during its
  own commands, and answers ATZ itself (CP: +CME ERROR: 100; first command of MM's enable
  sequence, fatal there). Other CP refusals seen (non-fatal): AT+CTZU=1, AT+CESQ=?, ATH (3).
- ModemManager 1.25.95, generic plugin: "Samsung LTE ALPSS", firmware N950FXXSGDUG6, port
  wwan0at0 (at). After enable: registered home, LTE, operator "eir" (Lidl Connect's host
  network), signal 29% (CSQ 9), packet service attached. Data still via mifmon (rmnet0);
  MM has no net port and would try PPP for a connect - do not let NM activate a mobile
  connection yet.
- Reboot test: everything up without intervention (modem enabled by a D-Bus client).
  rc-status shows modemmanager/polkit "failed": both were D-Bus-activated before their
  services started (pre-existing; daemons run fine).
- Service: MIFMON_MM=yes in /etc/conf.d/modem-ipc adds -M; modem-ipc starts before
  modemmanager.
- Lesson: `rc-service modemmanager stop` does not stop a D-Bus-activated ModemManager; an MM
  that outlives a mifmon restart waits forever on a lost reply - restart MM after mifmon.

## 2026-10-10 ~15:55 DATA THROUGH MODEMMANAGER: WRITTEN, NOT YET ON THE PHONE
(Laptop cloudflared tunnel went down ~13:30: Cloudflare 530 / "websocket: bad handshake";
the work below was done offline.)
- The generic MM plugin always dials ATD*99***cid# (PPP on the AT port); it never uses a
  net port. No existing plugin can be forced on a platform device (vendor id only from
  USB/PCI sysfs attributes; no udev override), so: new MM plugin "shannon"
  (tools/mm-plugin-shannon/0001-*.patch on MM tag 1.25.95-dev), matched by driver
  mif_sipc5, subsystems wwan+net. Bearer: dial = +CGACT=1,cid on the net port, IP =
  +CGCONTRDP=cid -> static (address, prefix, gateway, DNS), disconnect = +CGACT=0,cid
  (errors ignored, last PDN). Modem: DATA_NET only, no PPP.
- Compiled warning-free in the cloud against 1.25.95-dev (x86, MM's own -W set).
  WITH_SUSPEND_RESUME changes MMBaseBearerClass/MMBroadbandModemClass layouts and the phone's
  MM has it, so the .so must be built with -Dsystemd_suspend_resume=true:
  tools/mm-plugin-shannon/build-on-device.sh builds it on the phone (musl, same headers).
- mifmon -M -d = on-demand data call: attach as before, then wait (DC_ATTACHED); MM's
  AT+CGACT=1,1 runs the IPC define/activate and is answered OK when the IP config arrives;
  AT+CGACT? / AT+CGCONTRDP[=1] answered from the IPC state (address as /31, gateway = the
  other address of the pair, IPC DNS); AT+CGACT=0 -> OK (IPC call kept); cid != 1 ->
  +CME ERROR: 4 (only context 1 is routed to rmnet0). MTU from the IPC config set by
  ioctl. On CP power-up / call drop, want=0 so MM notices and reconnects. Unit-tested
  offline: replies fed to MM's mm_3gpp_parse_cgcontrdp_response -> /31, gw, both DNS.
- udev 78-greatlte-mm.rules: ID_MM_PHYSDEV_UID=shannon-cp on wwan0at0 and rmnet0 (rmnet0 has
  no parent device), rmnet1-7 ignored. modem-ipc: with MIFMON_MM=yes no rmnet-up hook.
- To test when the phone is reachable: install 78-greatlte-mm.rules, run
  build-on-device.sh, new mifmon, MIFMON_DATA=yes MIFMON_MM=yes, restart modem-ipc then
  ModemManager; expect plugin "shannon", ports wwan0at0 (at) + rmnet0 (net);
  `mmcli -m any --simple-connect=apn=` -> bearer connected, static IP on rmnet0;
  then NetworkManager's mobile connection / the UI toggle.

## 2026-10-11 ~23:15 MOBILE DATA THROUGH MODEMMANAGER + NETWORKMANAGER WORKS
(Tunnel back as access.oakdalehome.uk.)
- Phone MM = Alpine modemmanager-1.25.95_git20260709 = commit d776ea38 (aports 04eedda7),
  built with -Dsystemd_suspend_resume=true, polkit permissive, qmi/mbim/qrtr. The plugin
  patch applies unchanged; build-on-device.sh now fetches that exact commit (MM_COMMIT) and
  needs polkit-dev. Plugin built on the phone: /usr/lib/ModemManager/libmm-plugin-shannon.so.
- Fixes found on the device:
  - MM filtered rmnet0 as "virtual device" (no parent) -> "Failed to find a data port".
    ID_MM_DEVICE_PROCESS=1 on rmnet0 (explicit allowlist is checked before the virtual
    rule) in 78-greatlte-mm.rules.
  - MM's default connect uses ip-type IP; context 1 is IPV4V6, so MM created context 2 and
    sent AT+CGACT=1,2 (mifmon said +CME ERROR: 4). mifmon now maps any MM context onto the
    IPC call of context 1 (rmnet0) and echoes MM's number in +CGACT?/+CGCONTRDP.
- Result: `mmcli --simple-connect=apn=` -> bearer connected, interface rmnet0, static
  /31 + gateway + carrier DNS. NetworkManager gsm connection "Mobile data" (apn "",
  route metric 700, ipv6 off, autoconnect): rmnet0 configured, DNS in resolv.conf; with
  Wi-Fi off, HTTP by hostname over LTE works (loc=IE). After a reboot: connected <1 min
  after boot without intervention.
- Incident: the phone reset once (22:50), seconds after the plugin build finished and
  files were installed; every file written in that minute came back 0 bytes (no sync).
  pstore record undecodable (.enc.z). Not reproduced through several later CP restarts and
  udev triggers; kmsg showed "Could not lock PLL fout_cpucl0/1_pll" under load (cpufreq) -
  suspect the CPU clock path under heavy load rather than the modem. Always `sync` after
  installing on this phone.
- Debugging MM: it is D-Bus-activated (Exec=/usr/sbin/ModemManager) whenever something asks,
  so `rc-service modemmanager` loses the bus name; for debug logs temporarily add
  `--log-level=DEBUG --log-file=/var/log/mm.log` to the Exec line of
  /usr/share/dbus-1/system-services/org.freedesktop.ModemManager1.service.

## 2026-10-12 ~10:20 HARDWARE VIDEO DECODER (MFC v11) WORKS
- Kernel: `exynos8895-mfc.patch` (linux 7.0-r15; CONFIG_VIDEO_SAMSUNG_S5P_MFC=m). Details,
  findings and why each piece exists: BRINGUP.md "Hardware video codec: MFC v11".
- Phone: boot-v194-mfc.img (Image from the laptop tree before two cosmetic-only edits:
  a comment and the SysMMU "raw version" line demoted to dev_dbg; dtb = v186 dtb +
  tools/mfc/mfc_dtb_merge.py + mfc_iova_dt.py). Module /lib/modules/7.0.0/.../s5p-mfc.ko.zst
  is the final patch build. Firmware /lib/firmware/s5p-mfc-v11.fw = stock mfc_fw.bin.
  s5p_mfc autoloads; udev's v4l_id boots the firmware at ~5.5 s.
- Verified on a normal boot as the user: 1080p H.264 at ~280 fps via
  `ffmpeg -c:v h264_v4l2m2m`, 720p GStreamer decode bit-identical to software.
- Bring-up cost several freezes, all from the MFC power domain: power-down needs the TOP
  MFC bus clock and the block gates running (else it sticks half off and the next access
  hangs the bus), and after the firmware has run it refuses to power down at all. Now the
  domain powers off only before first use; after that the firmware stays resident.
  Debug method that worked: a step-trace with msleep between stages + `dmesg -w` streamed
  to the laptop (pstore keeps nothing after a forced reboot); PMU writes via devmem to
  test the domain alone. NEVER read /sys/kernel/debug/clk/clk_summary while the domain
  may be half off: it runtime-resumes CMU_MFC and hangs.
- Open: domain power-down after use (vendor sequence not found; costs idle leakage while
  the module is loaded); GStreamer copies frames out of uncached buffers (zero-copy needs
  a DMA-BUF consumer).

## 2026-10-12 ~10:45 MFC CLOSED OUT: ALL TESTABLE CODECS WORK, PHONE ON v195
- HEVC/VP9 (and MPEG-2) failed because the header buffer, which also holds the first key
  frame, was dropped after the header parse (mainline keeps it only for H.264). Fixed for
  v11 as the vendor driver does. Now bit-identical to software: H.264, HEVC, VP9, VP8;
  MPEG-2 73 dB, MPEG-4 57 dB, H.263 69 dB (non-bit-exact IDCT); H.264 encode works (NV21
  input, 40.7 dB at 4 Mb/s). Matrix and caveats: BRINGUP.md MFC section.
- boot-v195-mfc.img flashed = Image from the committed patch (+ same dtb as v194);
  module = committed patch. Verified after a cold boot: autoload, firmware boot at ~5.6 s,
  1080p H.264 at 280 fps.
- Test streams are synthetic (ffmpeg -f lavfi -i testsrc2, 720p, 5 s per codec).
- Still untested: VC-1 decode (no encoder to make a stream), HEVC/VP8/MPEG-4/H.263 encode.

## 2026-10-12 ~11:50 MFC POWER-DOWN AFTER USE AND GSTREAMER ZERO-COPY FIXED (v196)
- Power-down after use: the bootloader enables the MFC Q-channels (QCH_CON 0x003f0003,
  CMU option 0x10000000); the first power cycle reset them to disabled and every later
  power-down failed. CMU_MFC now saves/restores them (found by dumping CMU_MFC with
  devmem before and after a cycle). The "firmware resident" workaround is gone: firmware
  boots per first open, MFC_OFF=1 + domain off on last close (vendor lifecycle).
- Then a warm re-open race showed up: the domain goes off asynchronously, an open in
  that window hit a halted codec (synchronous external abort reading 0x7018, or a hang).
  The last close now waits for genpd to report the domain off (mutex held). Stress: 50
  rapid opens, 20 rotating-codec sessions, parallel decodes, 1080p - no errors, domain
  off after each session.
- GStreamer "copy" was the test: fakesink lacks GstVideoMeta support for NV12M. waylandsink
  and glimagesink are zero-copy (1080p real-time in KWin: 0.8 s CPU per 20 s);
  fakevideosink for benchmarks.
- Phone: boot-v196-mfc.img + final module; s5p_mfc autoloads. Bring-up blacklists removed.
- Note for future debugging: `pkill -f` with a pattern that also appears in the same
  ssh command line kills that command; kill the dmesg streamer separately.

## 2026-10-12 ~13:30 YOUTUBE ON THE MFC; APPS; FIREFOX CAN'T USE IT
- Firefox: no HW decode. Probe needs NV12/YV12 capture (MFC: NM12/NM21) and ffmpeg
  v4l2m2m with DRM_PRIME output (not in upstream ffmpeg). Details in BRINGUP.md.
- YouTube 1080p60 H.264 failed with MFC error 0x41: GStreamer gets s5p-mfc's default
  256 KiB stream buffer, too small for a 1080p60 key frame. v11 now always uses the 3 MiB
  CPB (patch updated, module rebuilt and reloaded live). 1080p60 H.264 and VP9 stream in
  real time on the MFC (10-18% of a core incl. HTTPS).
- Installed: clapper, clapper-enhancers, libpeas2-python3 (yt-dlp plugin is Python),
  pipeline (binary: tubefeeder), yt-dlp. Clapper crashed in libsoup (souphttpsrc);
  ~/.config/plasma-workspace/env/gst-http.sh makes curlhttpsrc preferred (next login).
- App playback not yet seen on screen: the phone was locked (kscreenlocker_greet), and
  Clapper's GTK sink waits for its window to render. Retest unlocked.
- Unrelated: a panfrost WARN (panfrost_gem.c:213, dmabuf import from a QtWebEngine app).

## 2026-10-12 ~13:50 YOUTUBE PLAYS IN CLAPPER ON THE MFC
- Clapper + yt-dlp plugin: YouTube plays with v4l2h264dec, adaptive 240p-720p60, no
  driver errors, 3-minute soak without a crash. Pipeline uses the same engine (not driven
  remotely; try it from the app drawer after re-login).
- Needed (all in tools/mfc/setup-youtube-apps.sh, applied on the phone):
  /usr/lib/libgstclapper.so symlink (Clapper's GL importers could not load -> no video
  sink); libpeas2-python3; curlhttpsrc over souphttpsrc; enhancers dir without the
  media scanner (CLAPPER_ENHANCERS_PATH) - its second pipeline crashed libsoup.
  Session env: ~/.config/plasma-workspace/env/gst-http.sh (next login).
- s5p-mfc fixes from GStreamer renegotiation: capture queue state reset on REQBUFS(0),
  FIELD_NONE in TRY_FMT. Patch updated; module rebuilt and live on the phone.
- Caveats: legacy dashdemux cannot take the plugin's in-memory DASH manifest; the
  yt-dlp plugin's gschemas.compiled is missing (its settings, e.g. codec order, fall
  back to defaults: avc1 first).
