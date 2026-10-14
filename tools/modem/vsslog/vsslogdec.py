import struct, sys, re
img = open(sys.argv[1], "rb").read()
data = open(sys.argv[2], "rb").read()
MAIN_OFF, MAIN_LOAD, MAIN_SIZE = 0x20c0, 0x40010000, 0x024adeb8
def rd(a, n):
    if MAIN_LOAD <= a < MAIN_LOAD + MAIN_SIZE:
        o = MAIN_OFF + a - MAIN_LOAD; return img[o:o + n]
seen = {}
for m in re.finditer(rb"\x7e\x7f", data):
    r = data[m.start():m.start() + 0x44]
    if len(r) < 0x30: continue
    ts = struct.unpack_from("<I", r, 13)[0]; ptr = struct.unpack_from("<I", r, 17)[0]
    e = rd(ptr, 32)
    if not e or e[:4] != b"DBT:": continue
    key = (ts, ptr, r[23:45])
    if key in seen: continue
    w = struct.unpack("<8I", e)
    fmt = rd(w[4], 300).split(b"\0")[0].decode("latin1")
    n = struct.unpack_from("<H", r, 23)[0]
    args = struct.unpack_from("<%dI" % min(n, 5), r, 25)
    seen[key] = (ts, fmt, args)
skip = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
for ts, fmt, args in sorted(seen.values()):
    if skip and skip.search(fmt): continue
    print(ts, fmt[:120], args)
