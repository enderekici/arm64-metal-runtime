#!/usr/bin/env python3
"""Turn scan-cache TSV into a markdown gap report.
usage: cache_gaps.py scan.tsv out.md [topN]"""
import os, re, subprocess, sys, tempfile, collections

tsv, out = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 60
imgs, words, total = [], {}, (0, 0)
for l in open(tsv):
    p = l.rstrip("\n").split("\t")
    if p[0] == "I": imgs.append((p[1], int(p[2]), int(p[3]), int(p[4])))
    elif p[0] == "T": total = (int(p[1]), int(p[2]))
    elif p[0] == "W":
        words[int(p[1], 16)] = (int(p[2]), [(x.rsplit(":", 1)[0], int(x.rsplit(":", 1)[1])) for x in p[4:]])

def disasm(ws):
    d = tempfile.mkdtemp()
    s = os.path.join(d, "a.s"); o = os.path.join(d, "a.o")
    open(s, "w").write(".text\n" + "".join(f".long 0x{w:08x}\n" for w in ws))
    subprocess.run(["clang", "-target", "arm64e-apple-macos", "-c", s, "-o", o], check=True)
    r = subprocess.run(["otool", "-arch", "arm64e", "-tV", o], capture_output=True, text=True).stdout.splitlines()
    res = {}
    for l in r:
        m = re.match(r"^[0-9a-f]+\t(\S+)\s*(.*)$", l)
        if m: res[len(res)] = (m.group(1), m.group(2))
    return [res.get(i, ("?", "")) for i in range(len(ws))]

ws = sorted(words)
dis = {}
for i in range(0, len(ws), 20000):
    chunk = ws[i:i + 20000]
    for w, d in zip(chunk, disasm(chunk)): dis[w] = d

def datalike(w):
    return w < 0x10000 or (w >> 16) in (0xffff, 0x7fff, 0x3f80, 0x4000) or dis[w][0] in (".long", "?", "udf")
def key(w):
    mn = dis[w][0]
    if mn in (".long", "?", "udf"): return ("data:" + ("udf" if mn == "udf" else "undef"), 0)
    return (mn, w & 0xFFE0FC00)

groups = collections.defaultdict(lambda: [0, 0, 0, collections.Counter(), None])
for w, (c, il) in words.items():
    g = groups[key(w)]
    g[0] += c; g[1] += 1
    if datalike(w): g[2] += c
    for n, k in il: g[3][n] += k
    if g[4] is None or c > words[g[4]][0]: g[4] = w
rows = sorted(groups.items(), key=lambda kv: -kv[1][0])

with open(out, "w") as f:
    f.write("# Shared-cache opcode gaps\n\n")
    f.write(f"Scanner: cpp/tools/scan_cache.cpp (decode_process, cached=true, empty import maps) over the executable sections of {len(imgs)} images; "
            f"cpp/tools/cache_gaps.py groups. Mnemonics from `otool -tV` on arm64e. Words are not filtered for data-in-code; "
            f"`undef` = otool cannot disassemble (almost always data or a pad), `data-like` count = words < 0x10000, 0xffff....-style, or undefined.\n\n")
    f.write(f"Total words scanned: {total[0]}; unsupported: {total[1]} ({100*total[1]/max(total[0],1):.2f}%).\n\n")
    f.write("## Unsupported fraction per image (top 40 by unsupported count)\n\n| image | words | unsupported | % | other errors |\n|---|---|---|---|---|\n")
    for n, w, u, o in sorted(imgs, key=lambda x: -x[2])[:40]:
        f.write(f"| {n.split('/')[-1] if n.count('/')>3 else n} | {w} | {u} | {100*u/max(w,1):.2f} | {o} |\n")
    f.write("\n## Top classes (grouped by mnemonic and opcode bits 31:21,15:10)\n\n")
    f.write("| # | count | distinct | data-like | class mask | example | decode | dominant image |\n|---|---|---|---|---|---|---|---|\n")
    for i, ((mn, mask), (c, dn, dl, im, ex)) in enumerate(rows[:top], 1):
        m, ops = dis[ex]
        dom = im.most_common(1)[0]
        f.write(f"| {i} | {c} | {dn} | {dl} | {mask:08x} | {ex:08x} | {m} {ops} | {dom[0].split('/')[-1]} ({dom[1]}) |\n")
    f.write("\n## Real-looking classes only (data-like count excluded)\n\n| # | real count | mnemonic | example | decode |\n|---|---|---|---|---|\n")
    real = sorted(((c - dl, mn, ex) for (mn, mask), (c, dn, dl, im, ex) in rows), reverse=True)
    for i, (c, mn, ex) in enumerate(real[:30], 1):
        f.write(f"| {i} | {c} | {mn} | {ex:08x} | {dis[ex][0]} {dis[ex][1]} |\n")
