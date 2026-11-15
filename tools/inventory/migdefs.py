#!/usr/bin/env python3
"""Parse SDK MIG .defs into routine tables. usage: migdefs.py OUT.json
Subsystem base + position (skip consumes an id). Records params (name, direction) so the flavor-like
parameter's C argument index can be found when every preceding parameter is a scalar."""
import glob, json, os, re, subprocess, sys
sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
inc = sdk + "/usr/include"
files = sorted(glob.glob(inc + "/mach/*.defs") + glob.glob(inc + "/servers/*.defs"))
out = {}
FLAVOR_NAMES = {"flavor", "which", "which_port", "op", "selector", "info_flavor", "port_type", "special_port_id", "node"}
for f in files:
    if os.path.basename(f) in ("mach_types.defs", "std_types.defs", "clock_types.defs"): continue
    src = subprocess.run(["xcrun", "clang", "-E", "-P", "-x", "c", "-I", inc, "-I", inc + "/mach", f],
                         capture_output=True, text=True).stdout
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    m = re.search(r"\bsubsystem\b(.*?);", src, re.S)
    if not m: continue
    toks = m.group(1).split()
    nums = [t for t in toks if t.isdigit()]
    if not nums: continue
    base = int(nums[-1]); name = [t for t in toks if not t.isdigit() and t not in ("KernelServer", "KernelUser")][0]
    idx = 0
    for st in re.finditer(r"\b(simpleroutine|routine|skip)\b\s*([A-Za-z_0-9]*)\s*(\((.*?)\))?\s*;", src[m.end():], re.S):
        kind, rname, _, params = st.groups()
        if kind == "skip":
            idx += 1; continue
        plist = []
        for p in (params or "").split(";"):
            p = p.strip()
            if not p: continue
            pm = re.match(r"((?:in|out|inout|requestport|replyport|sreplyport|pointer_t|waittime|msgoption|msgseqno|sendtime|ureplyport|userprefix|serverprefix|\s)*)\s*([A-Za-z_0-9]+)\s*:\s*(.*)", p, re.S)
            if pm: plist.append((pm.group(2), " ".join(pm.group(3).split()), pm.group(1).strip()))
        out[rname] = {"id": base + idx, "subsystem": name, "base": base, "params": plist}
        idx += 1
json.dump(out, open(sys.argv[1], "w"), indent=0)
print(len(out), "routines", sorted({v["subsystem"] for v in out.values()}))
