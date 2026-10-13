#!/usr/bin/env python3
"""Waydroid hwcomposer.waydroid.so (lineage-20, sha256 c5847fd6...): drop
windowsMutex in keyboard_handle_enter before the synchronous HIDL call
IWaydroidTask::setFocusedTask, which deadlocks against system_server holding
the WindowManager lock while calling IWaydroidWindow::setIdleInhibit."""
import hashlib, struct, sys

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
assert hashlib.sha256(d).hexdigest().startswith("c5847fd6b1228afb"), "unexpected input"

def w(addr):            # text segment: file offset == vaddr
    return struct.unpack_from("<I", d, addr)[0]
def put(addr, insn):
    struct.pack_into("<I", d, addr, insn)
def b(pc, t):  return 0x14000000 | (((t - pc) >> 2) & 0x3ffffff)
def bl(pc, t): return 0x94000000 | (((t - pc) >> 2) & 0x3ffffff)

UNLOCK, RSTAT_DTOR, EPILOGUE = 0x6bdb0, 0x6c0c0, 0x44970
CAVE = 0x6d360

# sanity: the original sequence after "bl stoi" at 0x44944
assert w(0x44944) == 0x94009e9f and w(0x44948) == 0xf9400288
assert w(0x4495c) == 0xd63f0120 and w(0x4496c) == 0x94009d11
assert d[CAVE:CAVE + 0x40] == bytes(0x40)

put(0x44948, 0x2a0003f5)              # mov w21, w0   (task id; x21 is callee-saved)
put(0x4494c, b(0x4494c, CAVE))        # b cave
for a in range(0x44950, 0x44968, 4):  # dead now; 0x44968 stays the unlock path
    put(a, 0xd503201f)                # nop

cave = [
    0xaa1303e0,                       # mov x0, x19          (&windowsMutex)
    ("bl", UNLOCK),                   # recursive_mutex::unlock
    0xf9400288,                       # ldr x8, [x20]        (task proxy vtable)
    0x2a1503e1,                       # mov w1, w21
    0xaa1403e0,                       # mov x0, x20
    0xf9403509,                       # ldr x9, [x8, #0x68]  (setFocusedTask)
    0x910003e8,                       # mov x8, sp           (Return<void>)
    0xd63f0120,                       # blr x9
    0x910003e0,                       # mov x0, sp
    ("bl", RSTAT_DTOR),               # ~return_status
    ("b", EPILOGUE),
]
pc = CAVE
for i in cave:
    if isinstance(i, tuple):
        i = (bl if i[0] == "bl" else b)(pc, i[1])
    put(pc, i); pc += 4

# grow the R+X PT_LOAD (index 2) to cover the cave
ph = 0x40 + 2 * 56
p_type, p_flags = struct.unpack_from("<II", d, ph)
assert p_type == 1 and p_flags == 5
filesz, memsz = struct.unpack_from("<QQ", d, ph + 32)
assert filesz == memsz == 0x33360
struct.pack_into("<QQ", d, ph + 32, 0x333a0, 0x333a0)

open(dst, "wb").write(d)
print("patched ->", dst, hashlib.sha256(d).hexdigest())
