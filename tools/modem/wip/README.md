# Work in progress: call audio (VSS)

`abox-vss-start.diff` (against the ABOX driver as patched by the audio
patches): maps the modem's real VSS memory into the ABOX window at IOVA
0xa0000000 instead of the dummy page (VSS = modem reserved memory
"exynos,modem_if" + shmem,cp_size, shmem,vss_size long: 0xfc600000, 6 MiB on
greatlte), adds a `start_vss` attribute that sends IPC_SYSTEM ABOX_START_VSS
(0xa0) as downstream does at MODEM_EVENT_ONLINE, and a `fw_log` parameter.
mifmon writes `start_vss` at every CP power-up (no-op without this module).

Not in the kernel patches: tested 2026-10-12, it does not help yet.
- The firmware does not ack ABOX_START_VSS within 1 ms (downstream only warns
  on that too), and afterwards stops answering IPCs (PCM opens time out); a
  module reload then fails with "no boot-done". A reboot recovers.
- The CP still crashes 20 s into a connected call (CP_CRASH_EXIT).
- No SysMMU faults are logged, map_pmu was off (downstream maps the PMU page
  0x16480000 and 0x10000000 for the firmware).

Leads: map the PMU for the firmware; the ext_bin blobs downstream loads into
the ABOX areas (DT: dsm.bin, AP_AUDIO_SLSI.bin, ...; the CP itself asks for
CP_AUDIO_SLSI.bin over RFS at boot); the call DMA channels (RDMA4, WDMA2,
type "call") and the HAL's mixer paths (reference/audio-config) for routing.
