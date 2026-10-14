# Call audio (VSS) bring-up notes

Solved 2026-10-13: see "Call audio" in docs/BRINGUP.md. The driver side is
now aports exynos8895-call-audio.patch (the experimental abox-vss.diff that
used to live here, minus the component, tuning-file and start_vss
experiments), the userspace side tools/callaudio. The notes below are the
history, kept for the dead ends.

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

## 2026-10-12 evening: downlink still silent (measured end to end)

- Phone warmth: Spectacle left over from screenshot work was spinning at 82%
  CPU, plus my silent test streams and a sleep inhibitor (CPU 95 C). Killed:
  59 C idle. Test scripts now start streams per test only.
- Stock HAL call sequence (reversed from audio.primary.universal8895.so,
  libaudio-ril.so, libvndsecril-client.so):
  1. "ABOX Audio Mode"=2. The downstream driver only stores it.
  2. incall_{nb|wb}-<dev> mixer paths.
  3. Open and start pcmC0D4p (RDMA4) and pcmC0D10c (WDMA2): 48 kHz, 2 ch,
     S16, period 160 x 2. 1024 zero bytes are written first.
  4. RIL calls, in order:
     - 09 03 [path,extra]: handset 01, speaker 06, headset 02
     - 09 01 [type,idx]: earpiece 01, speaker 11, headset 31; idx 0-5
     - 09 0d [00,00]: mode 0. [02,xx] is refused (0x8001).
     - 09 02 [00]: TX unmute
     - 09 02 [02]: RX unmute (03 = mute)
  The HAL sends no clock sync; realcall, WB-AMR and voice solution are stubs.
  Reproduced all of it, with the PCMs started before or after connect: no change.
- Measurements:
  - The VSS RDMA4 SRAM buffer is all zero. The AP can read ABOX SRAM: the
    vector table and IPC area read fine.
  - Mixer loopback: setting ROUTE_CTRL1 NSRC0 = 1 (SIFS0) after the capture
    starts (the driver resets it at capture start) records the mixer output.
    Validated with a 440 Hz tone (RMS 5657, as expected). During a call it
    records exact digital silence.
- VSS (in VSS memory, strings): decodes the speech itself ("UMTS AMR",
  "AMR WB FR"), has RX/TX mute counters (never logged), and gears
  100M/600M/1200M. Pages from VSS+0x520000 carry 0x3d-byte records during
  calls (an AMR-WB frame size?): frames may be flowing CP<->VSS.
- Open suspect: VSS runs capped at 590 MHz. Full speed (1.18 GHz) needs the
  INT bus/rail raised, as downstream does with pm_qos_int. Our kernel exposes
  no regulator for it (only vdd_mif), and ECT has no ABOX domain.

## 2026-10-13: solved

- Downlink: never broken. The silent results were test artifacts: recording
  before VSS established, and a stale "VHM_EstablishCall" flushed by turning
  on fw_log mid-call. (Correction, 10-14: L2C IS needed. It had been
  turned on by hand earlier that morning and survives warm reboots, so the
  "without L2C" control below still had it; see docs/BRINGUP.md.)
  Every control (tuning files, L2C, components, CSA0
  firmware, SND commands, firmware log never drained) still gives speech.
- Earpiece: AIF1RX1 -> HPOUT3L -> OUT3 demux HPOUT4, as stock. The test's
  own mixer recording tap (NSRC0 <- SIFS0) is what distorted it.
- Mics: dead because CONFIG_REGULATOR_ARIZONA_MICSUPP was off (MICVDD dummy,
  charge pump off), plus no DAPM widgets for them in the machine driver.
- Uplink choppy at gear 2 (590 MHz); clean at gear 1 with VDD_INT 800 mV.

