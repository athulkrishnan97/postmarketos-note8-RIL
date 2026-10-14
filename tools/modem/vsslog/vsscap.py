#!/usr/bin/env python3
# vsscap.py: from the next "call path armed" until "call path disarmed" in
# callaudio.log, copy the VSS log ring (VSS +0x520000, 128 KiB) every 150 ms
# into /tmp/vsscap.bin (concatenated snapshots). Run as root.
import mmap, os, time
BASE, SIZE = 0xfcb20000, 0x20000
LOG = "/var/log/callaudio.log"
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, SIZE, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE)
n = sum(1 for _ in open(LOG))
print("waiting for a call", flush=True)
def new_lines():
    return open(LOG).readlines()[n:]
while not any("call path armed" in l for l in new_lines()):
    time.sleep(0.05)
print(time.strftime("%H:%M:%S"), "capturing", flush=True)
cnt = 0
with open("/tmp/vsscap.bin", "wb") as out:
    while True:
        out.write(m[:]); cnt += 1
        if cnt % 20 == 0 and any("call path disarmed" in l for l in new_lines()):
            break
        time.sleep(0.15)
print(time.strftime("%H:%M:%S"), "done,", cnt, "snapshots", flush=True)
