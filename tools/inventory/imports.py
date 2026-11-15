#!/usr/bin/env python3
"""Closure imports + service-name strings. usage: imports.py INVENTORY_DIR
Writes imports.json ({file: [imported symbols that libsystem_kernel exports]}), syscall_imports.txt (files importing
_syscall: its number is in a register at the call site and is not recovered) and strings.txt (com.apple.* strings
from the on-disk closure and from the dyld-cache images' __TEXT)."""
import glob, json, os, re, subprocess, sys
inv = sys.argv[1]
J = os.path.expanduser("~/Library/Java/JavaVirtualMachines/azul-25/Contents/Home")
files = [(J + "/bin/java", None)] + [(J + f"/lib/{n}.dylib", None) for n in
         ("libjli", "libjava", "libzip", "libverify", "libnio", "libnet", "libjsig", "libjimage")] + \
        [(J + "/lib/server/libjvm.dylib", None), ("/usr/bin/plutil", "arm64e"), ("/usr/bin/tar", "arm64e")]
kg = json.load(open(inv + "/kgraph.json"))
res, sysc, strs = {}, [], set()
for f, arch in files:
    cmd = ["dyld_info"] + (["-arch", arch] if arch else []) + ["-imports", f]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    syms = set()
    for l in out.splitlines():
        m = re.search(r"(_[A-Za-z0-9_$.]+)\s+\(from ", l)
        if m: syms.add(m.group(1))
    if "_syscall" in syms: sysc.append(f)
    res[f] = sorted(s for s in syms if s in kg)
    st = subprocess.run(["strings", "-a"] + (["-arch", arch] if arch else []) + [f], capture_output=True, text=True).stdout
    for s in st.splitlines():
        if "com.apple." in s and len(s) < 200: strs.add(f"{os.path.basename(f)}\t{s}")
for idx in glob.glob(inv + "/dsc/*.idx"):
    base = os.path.basename(idx)[:-4]
    for l in open(idx):
        if l.startswith("str "): strs.add(f"{base}\t{l[4:].strip()}")
json.dump(res, open(inv + "/imports.json", "w"), indent=0)
open(inv + "/syscall_imports.txt", "w").write("\n".join(sysc) + "\n")
open(inv + "/strings.txt", "w").write("\n".join(sorted(strs)) + "\n")
print({os.path.basename(k): len(v) for k, v in res.items()}, "syscall importers:", len(sysc), "strings:", len(strs))
