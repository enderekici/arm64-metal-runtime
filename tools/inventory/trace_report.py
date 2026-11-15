#!/usr/bin/env python3
"""Decode a tracer file. usage: trace_report.py NAMES.tsv TRACEFILE [OUT.json]
JSON: {"calls": {sym: {"count": n, "samples": [[x0..x5,lr]...]}}, "text": [lines], "images": [[name, base, slide]]}"""
import json, struct, sys
RING_OFF, RING_ENTRIES = 65536, 1 << 19
STR_OFF = RING_OFF + RING_ENTRIES * 64
names = [l.split("\t")[1].strip() for l in open(sys.argv[1])]
d = open(sys.argv[2], "rb").read()
nxt, stroff = struct.unpack_from("<QQ", d, 0)
calls = {}
for i, n in enumerate(names):
    c = struct.unpack_from("<I", d, 4096 + 4 * i)[0]
    if c: calls[n] = {"count": c, "samples": []}
for k in range(min(nxt, RING_ENTRIES)):
    fid, cnt, x0, x1, x2, x3, x4, x5, lr = struct.unpack_from("<IIQQQQQQQ", d, RING_OFF + 64 * k)
    if fid < len(names) and names[fid] in calls and (cnt or x0 or x1 or lr):
        calls[names[fid]]["samples"].append([x0, x1, x2, x3, x4, x5, lr])
text = d[STR_OFF:STR_OFF + stroff].decode(errors="replace").split("\n")
images = [l.split("\t")[1:] for l in text if l.startswith("image\t")]
text = [l for l in text if l and not l.startswith("image\t")]
res = {"calls": calls, "text": text, "images": images}
if len(sys.argv) > 3: json.dump(res, open(sys.argv[3], "w"))
else:
    for n, v in sorted(calls.items(), key=lambda kv: -kv[1]["count"]): print(v["count"], n)
    print("\n".join(text))
