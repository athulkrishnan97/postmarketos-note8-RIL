# Work in progress: call audio (VSS)

`abox-vss.diff`, against the ABOX driver as the audio patches leave it. It is
installed on the phone as a module with
`options snd_soc_samsung_abox fw_log=1 gear_min=2` in
/etc/modprobe.d/abox-debug.conf. The original module is in
/root/snd-soc-samsung-abox.ko.pre-vss.

What it does:
- Maps the modem's real VSS memory into the ABOX window at IOVA 0xa0000000
  instead of the dummy page. VSS is the modem reserved memory
  ("exynos,modem_if") + shmem,cp_size, shmem,vss_size long: 0xfc600000, 6 MiB.
  The Calliope firmware then starts VSS by itself at boot, once it finds the
  CP's magic word at VSS+0x500000: "CP Magic Code is ready MC=0xFACEBA98 ...
  VHM_initialize done". That only happens when the CP is online before the
  firmware boots, as in a normal boot. ABOX_START_VSS (0xa0) is not a message
  this firmware (CQK0) knows ("What is this message: 160"); start_vss stays
  behind the vss parameter and is off.
- gear_min: at call setup VSS asks for CPU gear 1 (the PLL rate, 1.18 GHz).
  With that, the firmware stops answering for good and the CP crashes 20 s
  into the call (CP_CRASH_EXIT). Capped at gear 2 (590 MHz), calls stay up
  indefinitely. Downstream raises the INT bus QoS with the gear, which we do
  not have.
- fw_log: the firmware's log as dev_info.
- call attribute: starts the "call" RDMA4 / WDMA2 as PCM tasks (open,
  buffer, params, prepare, trigger), as the stock HAL's call PCMs do. It also
  routes RDMA4 -> mixer -> UAIF0 and UAIF0 -> NSRC1 -> WDMA2. VSS then runs
  its own PCM threads ("UAIF_SPK_status 1", "UAIF_MIC_status 1") and points
  RDMA4/WDMA2 at its own SRAM buffers (RDMA4 0x13f21410-0x13f21b90, 10 ms of
  48 kHz stereo; WDMA2 0x13f20010-0x13f20290).

Tested 2026-10-12, 2G:
- Outgoing (own voicemail) and incoming calls (caller ID, answer): stable.
- Downlink silent: the RDMA4 buffer VSS fills stays exactly zero, even with
  the caller talking. SND path/volume/mute/clock (09 03/01/02/09, decoded from
  libsec-ril) were accepted (0x8000) but changed nothing.
- Uplink zero too: the codec microphones are not routed yet.
- The UAIF0 clock needs an open stream on the codec while a call is up. The
  test used silent aplay/arecord on hw:0,0.

Next: why VSS outputs silence. Candidates: the CP->VSS voice ring in the
VSS memory (+0x504000.. changes during calls), VSS volume/mute on the ABOX
side, and the stock HAL's exact call sequence (audio_hw for universal8895
and secril-client). Then the codec routes: earpiece out and main mic in.
