#!/usr/bin/env python3
"""Region call graph of libsystem_kernel (direct b/bl only; a region runs from one export to the next, so
unexported local helpers fold into the preceding export - results are 'may reach').
usage: kcallgraph.py DSC_DIR STATIC_SVC.json OUT.json   -> {sym: {"callees": [...], "svc": [nums]}}"""
import bisect, json, struct, sys
dsc, svcj, outp = sys.argv[1:4]
texts, exports = [], {}
for l in open(dsc + "/libsystem_kernel.dylib.idx"):
    p = l.rstrip("\n").split(" ", 3)
    if p[0] == "text": texts.append((int(p[1]), int(p[2]), int(p[3])))
    elif p[0] == "export": exports.setdefault(int(p[1]), []).append(p[2])
data = open(dsc + "/libsystem_kernel.dylib.bin", "rb").read()
addrs = sorted(a for a in exports if any(s <= a < s + z for s, _, z in texts))
def region(a):
    k = bisect.bisect_right(addrs, a) - 1
    return addrs[k] if k >= 0 else None
graph = {}
for s, off, size in texts:
    words = struct.unpack("<%dI" % (size // 4), data[off:off + size])
    for i, w in enumerate(words):
        if (w >> 26) in (0b000101, 0b100101):
            imm = w & 0x3FFFFFF
            if imm >> 25: imm -= 1 << 26
            tgt = s + 4 * i + imm * 4
            src, dst = region(s + 4 * i), region(tgt)
            if src is not None and dst is not None and src != dst:
                graph.setdefault(src, set()).add(dst)
svc = {}
for r in json.load(open(svcj)):
    if r["image"].endswith("libsystem_kernel.dylib") and r["num"] is not None:
        a = region(r["addr"])
        svc.setdefault(a, set()).add(r["num"])
res = {}
for a, names in exports.items():
    if a not in addrs: continue
    for n in names:
        res[n] = {"callees": sorted({m for d in graph.get(a, ()) for m in exports.get(d, ())}),
                  "svc": sorted(svc.get(a, ())), "addr": a}
json.dump(res, open(outp, "w"))
print(len(res), "regions with", sum(1 for v in res.values() if v["svc"]), "svc")
