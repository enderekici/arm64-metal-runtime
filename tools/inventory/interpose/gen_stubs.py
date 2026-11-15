#!/usr/bin/env python3
"""Generate counting/sampling interpose stubs for every function exported by libsystem_kernel.
usage: gen_stubs.py DSC_DIR OUT_DIR [--exclude NAME...]
Writes OUT_DIR/stubs.S (stubs + __DATA,__interpose table) and OUT_DIR/names.tsv (id<TAB>symbol)."""
import sys
dsc, out = sys.argv[1], sys.argv[2]
extra_exclude = set(sys.argv[4:]) if len(sys.argv) > 3 and sys.argv[3] == "--exclude" else set()
# functions wrapped in C (interpose.c) instead of generic stubs
C_WRAPPED = {"_mach_msg", "_mach_msg_overwrite", "_mach_msg2_internal"}
texts, exports = [], {}
for l in open(dsc + "/libsystem_kernel.dylib.idx"):
    p = l.rstrip("\n").split(" ", 3)
    if p[0] == "text": texts.append((int(p[1]), int(p[1]) + int(p[3])))
    elif p[0] == "export": exports[p[2]] = int(p[1])
names = sorted(n for n, a in exports.items() if any(s <= a < e for s, e in texts) and n not in C_WRAPPED
               and n not in extra_exclude)
# hot, uninteresting, non-syscall commpage helpers: skip to keep sampling cheap and avoid recursion
SKIP = {"_mach_absolute_time", "_mach_continuous_time", "_mach_approximate_time", "_mach_continuous_approximate_time"}
names = [n for n in names if n not in SKIP]
s = ['.arch armv8.1-a', '.text', '.align 2']
tab = ['.section __DATA,__interpose']
for i, n in enumerate(names):
    s += [f"_trace_stub_{i}:",
          "  adrp x9, _g_state@PAGE", "  ldr x9, [x9, _g_state@PAGEOFF]", "  cbz x9, 9f",
          f"  movz x10, #{4096 + 4 * i}", "  add x10, x9, x10", "  mov w11, #1",
          "  ldaddal w11, w15, [x10]", "  cmp w15, #256", "  b.hs 9f",
          "  mov x12, #1", "  ldaddal x12, x12, [x9]", "  and x12, x12, #0x7ffff",
          "  add x13, x9, #16, lsl #12", "  add x13, x13, x12, lsl #6", f"  mov w14, #{i}",
          "  stp w14, w15, [x13]", "  stp x0, x1, [x13, #8]", "  stp x2, x3, [x13, #24]",
          "  stp x4, x5, [x13, #40]", "  str x30, [x13, #56]", f"9: b {n}"]
    tab += ["  .align 3", f"  .quad _trace_stub_{i}", f"  .quad {n}"]
open(out + "/stubs.S", "w").write("\n".join(s + tab) + "\n")
open(out + "/names.tsv", "w").write("".join(f"{i}\t{n}\n" for i, n in enumerate(names)))
print(len(names), "stubs")
