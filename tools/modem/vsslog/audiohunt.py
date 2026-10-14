#!/usr/bin/env python3
# audiohunt2.py: like audiohunt.py, but saves the snapshots and scores blocks
# by lag-1 autocorrelation of int16 samples (mono and stereo), so only
# audio-like data is reported. Run as root.
import mmap, os, time, array, math, pickle
REGIONS = [("sram", 0x13f00000, 0x31000), ("vss", 0xfc600000, 0x600000)]
LOG = "/var/log/callaudio.log"
n = sum(1 for _ in open(LOG))
print("waiting for a call", flush=True)
while not any("sending SND setup" in l for l in open(LOG).readlines()[n:]):
    time.sleep(0.3)
time.sleep(4)
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
maps = {name: mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
        for name, base, size in REGIONS}
snaps = []
for k in range(4):
    snaps.append({name: m[:] for name, m in maps.items()})
    time.sleep(0.1)
print(time.strftime("%H:%M:%S"), "snapshots taken", flush=True)
pickle.dump(snaps, open("/tmp/audiohunt2.pkl", "wb"))
def corr(a, step):
    x = a[::step]
    if len(x) < 8: return 0
    m = sum(x) / len(x)
    num = sum((x[i] - m) * (x[i + 1] - m) for i in range(len(x) - 1))
    den = sum((v - m) ** 2 for v in x) or 1
    return num / den
for name, base, size in REGIONS:
    hits = []
    for off in range(0, size, 1024):
        blocks = [s[name][off:off + 1024] for s in snaps]
        if all(b == blocks[0] for b in blocks):
            continue
        a = array.array("h", blocks[-1])
        rms = math.sqrt(sum(x * x for x in a) / len(a))
        if rms < 20:
            continue
        c = max(corr(a, 1), corr(a, 2))
        if c > 0.6:
            hits.append((off, rms, c))
    print(f"{name}: {len(hits)} changing audio-like blocks")
    for off, rms, c in hits[:40]:
        print(f"   {base + off:#x} (+{off:#x}) rms {rms:.0f} corr {c:.2f}")
print("done", flush=True)
