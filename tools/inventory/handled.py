#!/usr/bin/env python3
"""Extract what the runtime source handles. usage: handled.py WORKTREE OUT.json
Lexical and brace-aware (no execution): BSD syscall numbers (syscall / syscall_ / netSyscall tokens and
switch cases on them), Mach trap numbers (machTrapNumber and the negative-syscall casts), kernel-object
MIG ids the dispatcher models (messageId tokens), the kernelObjectRoutineExists ranges (replied
KERN_NOT_SUPPORTED, NOT handled), csops ops, fcntl commands, and host_service_registry symbols.
A BSD number is 'stub' when its handler's terminal statement is unsupported()/ENOSYS/ENOTSUP/EOPNOTSUPP;
that classification is heuristic - read the site (the 'where' list) before trusting it."""
import json, os, re, subprocess, sys

wt, out = sys.argv[1], sys.argv[2]
sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
sysnum = {}
for l in open(sdk + "/usr/include/sys/syscall.h"):
    m = re.match(r"#define\s+SYS_(\w+)\s+(\d+)", l)
    if m: sysnum["SYS_" + m.group(1)] = int(m.group(2))
rev = {}
for k, v in sysnum.items(): rev.setdefault(v, k[4:])

def num(tok):
    tok = tok.strip()
    if tok in sysnum: return sysnum[tok]
    try: return int(tok, 0)
    except ValueError: return None

FILES = ["cpp/process_runner.cpp", "cpp/raw_proc_syscalls.cpp", "cpp/raw_fs_syscalls.inc",
         "cpp/raw_net_syscalls.inc", "cpp/raw_net_state.inc"]
SW = {"syscall", "syscall_", "netSyscall"}
STUB_RE = re.compile(r"unsupported\(\)|\bENOSYS\b|\bENOTSUP\b|\bEOPNOTSUPP\b")
bsd, mach, mig = {}, {}, {}

def add(d, n, where, stub=False):
    if n is None: return
    e = d.setdefault(n, {"where": [], "stub_sites": 0, "other_sites": 0})
    e["where"].append(where)
    e["stub_sites" if stub else "other_sites"] += 1

def strip(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    return re.sub(r"//.*", "", line)

def terminal_is_stub(lines, i, f):
    # Only the raw_* handlers use the dedicated stub idioms; read up to the first return/continue.
    if not f.startswith("cpp/raw_"): return False
    buf = []
    for l in lines[i - 1:i + 6]:
        buf.append(strip(l))
        if re.search(r"\b(return|continue)\b\s*;", l) and not re.search(r"^\s*case\b[^;]*:\s*$", l): break
    text = " ".join(buf)
    text = re.sub(r"^\s*if \((?:[^{]*(?:syscall_?|netSyscall)[^{]*)\)\s*\{?", "", text)  # the dispatch guard itself
    m = STUB_RE.search(text)
    # an if/else before the failing statement makes it conditional (the call has a working path)
    return bool(m) and not re.search(r"\bif\b|\belse\b", text[:m.start()].split("case", 1)[-1])

for f in FILES:
    lines = open(os.path.join(wt, f), errors="replace").read().split("\n")
    stack, depth = [], 0
    for i, raw in enumerate(lines, 1):
        line = strip(raw)
        sm = re.search(r"switch \(([^)]*)\)\s*\{", line)
        if sm: stack.append((depth, sm.group(1).strip()))
        top = stack[-1][1] if stack else None
        stub = terminal_is_stub(lines, i, f)
        for m in re.finditer(r"\bcase\s+([A-Za-z_0-9]+)\s*:", line):
            n = num(m.group(1))
            # raw_proc run(): routing table to sub-dispatchers; the sub-dispatcher's own case is the handler
            if f.endswith("raw_proc_syscalls.cpp") and "RawProcOutcome Dispatcher::run" in "\n".join(lines[max(0, i - 60):i]) and i > 1205:
                continue
            if top in SW: add(bsd, n, f"{f}:{i}", stub)
            elif top == "machTrapNumber": add(mach, n, f"{f}:{i}")
            elif top == "messageId": add(mig, n, f"{f}:{i}")
        for m in re.finditer(r"\b(syscall_?|netSyscall)\s*(?:==|!=)\s*([A-Za-z_0-9]+)", line):
            add(bsd, num(m.group(2)), f"{f}:{i}", stub)
        for m in re.finditer(r"syscall\s*==\s*static_cast<std::uint64_t>\(static_cast<std::int64_t>\(-(\d+)\)\)", line):
            add(mach, int(m.group(1)), f"{f}:{i}")
        for m in re.finditer(r"\bmachTrapNumber\s*(?:==|!=)\s*(\d+)", line):
            add(mach, int(m.group(1)), f"{f}:{i}")
        for m in re.finditer(r"\bmessageId\s*(?:==|!=)\s*(\d+)", line):
            add(mig, int(m.group(1)), f"{f}:{i}")
        if re.search(r"syscall\s*>=\s*234\s*&&\s*syscall\s*<=\s*241", line):
            for n in range(234, 242): add(bsd, n, f"{f}:{i}")
        depth += line.count("{") - line.count("}")
        while stack and depth <= stack[-1][0] and "}" in line: stack.pop()

src = open(os.path.join(wt, "cpp/process_runner.cpp")).read()
m = re.search(r"kernelObjectRoutineExists = .*?ranges\[\] = \{(.*?)\};", src, re.S)
ranges = [(int(a), int(b)) for a, b in re.findall(r"\{(\d+),\s*(\d+)\}", m.group(1))]
csops = sorted({int(x) for x in re.findall(r"\bcase\s+(\d+):", open(os.path.join(wt, "cpp/guest_csops.hpp")).read())})

# fcntl commands: names in the switch(command) of the fcntl handler -> numbers via the SDK header
net = open(os.path.join(wt, "cpp/raw_net_syscalls.inc")).read().split("\n")
start = next(i for i, l in enumerate(net) if "switch (command)" in l and i > 1300)
fnames = set()
for l in net[start:start + 220]:
    for n in re.findall(r"\bcase (F_[A-Z_0-9]+):", l): fnames.add(n)
    for n in re.findall(r"command == (F_[A-Z_0-9]+)", l): fnames.add(n)
fvals = {}
for l in open(sdk + "/usr/include/sys/fcntl.h"):
    mm = re.match(r"#define\s+(F_[A-Z_0-9]+)\s+(\d+)\b", l)
    if mm: fvals[mm.group(1)] = int(mm.group(2))
fcntl = {n: fvals.get(n) for n in sorted(fnames)}

# host-service API intercepts live for the runner (shared-cache hostExportAddresses uses host_service_ids())
reg = open(os.path.join(wt, "cpp/host_service_registry.hpp")).read()
host = {s: {"id": int(i), "runner": r == "true"} for s, i, d, r in
        re.findall(r'\{"(_[A-Za-z0-9_$]+)",\s*(\d+),\s*(true|false),\s*(true|false)\}', reg)}

# acct/reboot/nfssvc share a case group with swapon's unsupported(); they deny (EPERM), checked by hand
for n in (51, 55, 155):
    if n in bsd: bsd[n]["other_sites"] += bsd[n]["stub_sites"]; bsd[n]["stub_sites"] = 0
# swapon: `if (syscall_ == 85) unsupported();` is its only body (conditional on the shared case group)
if 85 in bsd: bsd[85]["stub_sites"] = max(1, bsd[85]["stub_sites"]); bsd[85]["other_sites"] = 0

json.dump({"bsd": {k: v for k, v in sorted(bsd.items())}, "mach": {k: v for k, v in sorted(mach.items())},
           "mig_handled": {k: v for k, v in sorted(mig.items())}, "mig_exists_ranges": ranges,
           "csops_ops_modelled": csops, "fcntl_cmds_modelled": fcntl, "host_services": host,
           "sysnames": {str(k): v for k, v in rev.items()}}, open(out, "w"), indent=1)
stub = sorted(k for k, v in bsd.items() if v["stub_sites"] and not v["other_sites"])
print(f"bsd={len(bsd)} stub-only={stub} mach={len(mach)} mig_handled={sorted(mig)} ranges={len(ranges)} "
      f"csops={csops} fcntl={len(fcntl)} host_services={len(host)}")
