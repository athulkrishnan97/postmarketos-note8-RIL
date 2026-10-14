# VSS debug log

The modem's voice firmware (VSS), run by the ABOX's Calliope firmware, writes
a binary trace ring at VSS +0x520000 (phys 0xfcb20000, ~128 KiB) while a call
is up; it is idle otherwise. Records:

    7e 7f 40 00 00 3d 00 | seq(2) ... | time u32 @13 | trace ptr u32 @17 |
    00 00 | nargs u16 @23 | args u32[] @25

The trace pointer is a CP address in the RADIO image's MAIN section (TOC: MAIN
file_off 0x20c0, load 0x40010000): a "DBT:" entry whose word 4 is the format
string and word 6 the source file. So the RADIO partition (or the backup
image) decodes the log off the phone.

- vsscap.py (phone, root): from callaudio's "call path armed" until "call path
  disarmed", copies the ring every 150 ms to /tmp/vsscap.bin.
- vsslogdec.py RADIO.img vsscap.bin [skip-regex] (anywhere): dedupes and prints
  the records in time order with arguments.
- audiohunt.py (phone, root): at the next active call, snapshots ABOX SRAM and
  VSS memory and lists changing blocks that look like audio (int16 RMS and
  lag-1 autocorrelation).

Useful messages: "[VHM]ChnInitReq ... mute_tx/rx", "[VSS-VHM channel distroy]
Abox_vhmVocRxDecCnt" (frames decoded in the call), "[SFR] ..." register
dumps at channel init, "SetAudioRx/TxMuteState: MuteByAP/CP".
