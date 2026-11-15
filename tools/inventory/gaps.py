#!/usr/bin/env python3
"""Join native traces, static scans and the runtime's handled tables into inventory/gaps.md.
usage: gaps.py WORKDIR RUNS_DIR INVENTORY_DIR
  WORKDIR       logs/trace (holds ip/names.tsv)
  RUNS_DIR      one run_all.sh output dir (runs-<epoch>)
  INVENTORY_DIR inventory/ (static_svc.json kgraph.json mig.json handled.json imports.json strings.txt dsc/)
Everything not observed or scanned is absent, not 'clean'."""
import collections, glob, json, math, os, re, subprocess, sys

WORK, RUNS, INV = sys.argv[1:4]
J = lambda p: json.load(open(os.path.join(INV, p)))
static_svc, kgraph, mig, handled, imports = J("static_svc.json"), J("kgraph.json"), J("mig.json"), J("handled.json"), J("imports.json")
sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
names_tsv = [l.split("\t")[1].strip() for l in open(WORK + "/ip/names.tsv")]

TARGETS = {"java": ["java-version-a", "java-version-b"], "jni": ["jni-entry"], "plutil": ["plutil-lint"],
           "tar": ["tar-list", "tar-extract"]}
sysnames = handled["sysnames"]
bsd_h, mach_h = {int(k): v for k, v in handled["bsd"].items()}, {int(k): v for k, v in handled["mach"].items()}
mig_h = {int(k): v for k, v in handled["mig_handled"].items()}
ranges = [tuple(r) for r in handled["mig_exists_ranges"]]
host_rt = {s for s, v in handled["host_services"].items() if v["runner"]}
mig_by_id = {v["id"]: k for k, v in mig.items()}

def defines(path, prefix):
    d = {}
    if not os.path.exists(path): return d
    for l in open(path, errors="replace"):
        m = re.match(r"#\s*define\s+(%s\w*)\s+\(?(0x[0-9a-fA-F]+|\d+)\)?\s*(?:/|$)" % prefix, l)
        if m: d.setdefault(int(m.group(2), 0), m.group(1))
    return d
FCNTL = defines(sdk + "/usr/include/sys/fcntl.h", "F_")
PROCPID = defines(sdk + "/usr/include/sys/proc_info.h", "PROC_PID")
PROCSELF = defines(sdk + "/usr/include/sys/proc_info.h", "PROC_SELFSET_")
CALLNAMES = {1: "LISTPIDS", 2: "PIDINFO", 3: "PIDFDINFO", 4: "KERNMSGBUF", 5: "SETCONTROL", 6: "PIDFILEPORTINFO", 7: "TERMINATE",
             8: "DIRTYCONTROL", 9: "PIDRUSAGE", 10: "PIDORIGINATORINFO", 11: "LISTCOALITIONS", 12: "CANUSEFGHW", 13: "PIDDYNKQUEUEINFO",
             14: "UDATA_INFO", 15: "SET_DYLD_IMAGES", 16: "TERMINATE_RSR"}  # xnu proc_info.h PROC_INFO_CALL_*
FLAVNAMES = {}
for h, pre in (("mach/task_info.h", "TASK_"), ("mach/thread_info.h", "THREAD_"), ("mach/host_info.h", "HOST_"),
               ("mach/vm_region.h", "VM_REGION_"), ("mach/port.h", "MACH_PORT_"), ("mach/task_special_ports.h", "TASK_"),
               ("mach/thread_policy.h", "THREAD_"), ("mach/task_policy.h", "TASK_")):
    FLAVNAMES[h] = defines(sdk + "/usr/include/" + h, pre)
KERNN = defines(sdk + "/usr/include/sys/sysctl.h", "KERN_")
HWN = defines(sdk + "/usr/include/sys/sysctl.h", "HW_")
def mib_name(m):
    p = m.split(".")
    nm = {"1": "kern", "6": "hw", "2": "vm"}.get(p[0], p[0])
    if len(p) > 1:
        t = KERNN if p[0] == "1" else HWN if p[0] == "6" else {}
        nm += "." + t.get(int(p[1]), p[1]).lower().replace("kern_", "").replace("hw_", "")
    if m.startswith("1.14.1."): nm = "kern.proc.pid.<pid>"
    return nm + (" [" + ".".join(p[2:]) + "]" if len(p) > 2 and not m.startswith("1.14.1.") else "")
CSOPS = dict(enumerate("STATUS MARKINVALID MARKHARD MARKKILL PIDPATH CDHASH PIDOFFSET ENTITLEMENTS_BLOB MARKRESTRICT "
                       "SET_STATUS BLOB IDENTITY CLEARINSTALLER CLEARPLATFORM TEAMID CLEAR_LV DER_ENTITLEMENTS_BLOB "
                       "VALIDATION_CATEGORY".split()))  # xnu bsd/sys/codesign.h (header absent from the SDK)

# ---- per-run data -------------------------------------------------------------------------
def load_run(tag):
    f = glob.glob(f"{RUNS}/{tag}/raw.*.json")
    return json.load(open(f[0])) if f else None
runs = {t: [load_run(x) for x in tags] for t, tags in TARGETS.items()}
def image_of(run, lr):
    imgs = sorted((int(b, 16), n) for n, b, s in run["images"])
    lr &= 0x7FFFFFFFFF
    best = None
    for b, n in imgs:
        if b <= lr: best = n
    return os.path.basename(best) if best else "?"

# ---- API -> (kind, number) ----------------------------------------------------------------
MACHDEP = 0x80000000  # arm64 machine-dependent call (thread_set_tsd_base)
def wrapper_rows(api):
    """libsystem_kernel export -> list of (kind, num, via, note). Trap-first MIG stubs report the trap."""
    out = []
    k = kgraph.get(api)
    base = api[1:] if api.startswith("_") else api
    traps = [c for c in (k["callees"] if k else []) if c.endswith("_trap") and kgraph.get(c, {}).get("svc")]
    traps_n = [(c, n) for c in traps for n in kgraph[c]["svc"]]
    mname = base if base in mig else "_kernelrpc_" + base if "_kernelrpc_" + base in mig else None
    if mname:
        mid = mig[mname]["id"]
        if traps_n:
            for c, n in traps_n:
                out.append(("mach-trap", -n, c, f"MIG fallback id {mid} ({base})"))
        else:
            out.append(("mig-id", mid, api, mig[mname]["subsystem"]))
        return out
    if not k: return out
    seen, frontier = {api}, [api]
    for depth in range(4):
        nxt = []
        for a in frontier:
            for n in kgraph[a]["svc"]:
                out.append(("machdep" if n == MACHDEP else "mach-trap" if n < 0 else "bsd", MACHDEP if n == MACHDEP else abs(n) if n < 0 else n, a if depth else "", ""))
            for c in kgraph[a]["callees"]:
                if c not in seen: seen.add(c); nxt.append(c)
        if out: break
        frontier = nxt
    return out

def bsd_name(n): return sysnames.get(str(n), f"syscall{n}")
TRAP_NAMES = {}
for r in static_svc:
    if r["image"].endswith("libsystem_kernel.dylib") and r["num"] is not None and r["num"] < 0 and r["sym"]:
        TRAP_NAMES.setdefault(-r["num"], r["sym"].lstrip("_"))

# ---- row store ----------------------------------------------------------------------------
rows = {}
def row(kind, num, name=""):
    r = rows.setdefault((kind, num), {"kind": kind, "num": num, "name": name, "native": collections.Counter(),
                                       "callers": collections.Counter(), "static": set(), "apis": set(), "notes": set(),
                                       "missing": set(), "have": set(), "have_obs": set(), "samples": collections.Counter()})
    if name and not r["name"]: r["name"] = name
    return r

# ---- partial-implementation tables (read from the handlers; line refs in the 'notes') -------------
def flav_name(table, v):
    return FLAVNAMES.get(table, {}).get(v, str(v))
# mig id -> (arg index of selector in C signature, table, predicate(handled?), note)
SUBOP = {
 3405: (1, "mach/task_info.h", lambda f: f != 17, "process_runner.cpp messageId==3405 handler: TASK_DYLD_INFO(17) -> KERN_NOT_SUPPORTED; every other flavor is forwarded to the RUNNER's own task (values describe the host process, not the guest)"),
 200: (1, "mach/host_info.h", lambda f: True, "host_info forwarded to host for any flavor (messageId==200 handler)"),
 3409: (1, "mach/task_special_ports.h", lambda f: f in (1, 3, 4, 10), "task_get_special_port models selectors 1,3,4,10 only (messageId==3409 handler)"),
 3410: (1, "mach/task_special_ports.h", lambda f: f == 10, "task_set_special_port models selector 10 only (messageId==3410 handler)"),
 412: (2, "mach/host_special_ports.h", lambda f: False, "host_get_special_port: size-40 shape replies KERN_INVALID_ARGUMENT for every selector (messageId==412 branch in the kernel-object send path)"),
 3218: (2, "mach/port.h", lambda f: f == 1, "mach_port_set_attributes models MACH_PORT_LIMITS_INFO only (messageId==3218 branch in the kernel-object send path)"),
}

def kernel_status(kind, num):
    """(status, effect, note) for the runtime's treatment of a number."""
    if kind == "bsd":
        e = bsd_h.get(num)
        if not e: return "no", "ABORT: runtime throws 'unsupported Darwin syscall'", ""
        if e["stub_sites"] and not e["other_sites"]: return "stub", "returns ENOTSUP/ENOSYS/EOPNOTSUPP (heuristic)", ",".join(e["where"][:2])
        return "yes", "", ",".join(e["where"][:2])
    if kind == "machdep":
        return "no", "UNVERIFIED: no handler for x16=0x80000000 in the source; libSystem's initializer is modeled separately (HANDOFF), so reachability is unproven", ""
    if kind == "mach-trap":
        e = mach_h.get(num)
        if not e: return "no", "ABORT: unhandled trap falls through to the unsupported-syscall throw", ""
        return "yes", "", ",".join(e["where"][:2])
    if kind == "mig-id":
        if num in mig_h: return "yes", "", ",".join(mig_h[num]["where"][:2])
        if any(a <= num <= b for a, b in ranges): return "no", "KERN_NOT_SUPPORTED reply (kernelObjectRoutineExists)", ""
        return "no", "MIG_BAD_ID (-303) reply", ""
    return "no", "", ""

# ---- ingest native ------------------------------------------------------------------------
def ingest_api(tgt, run, api, rec):
    cnt = rec["count"]
    for kind, num, via, note in wrapper_rows(api):
        nm = bsd_name(num) if kind == "bsd" else "thread_set_tsd_base" if kind == "machdep" else TRAP_NAMES.get(num, f"trap{num}") if kind == "mach-trap" else mig_by_id.get(num, "?").replace("_kernelrpc_", "")
        r = row(kind, num, nm)
        r["native"][tgt] += cnt; r["apis"].add(api)
        if note: r["notes"].add(note)
        if via: r["notes"].add(f"via {via}")
        for s in rec["samples"][:256]:
            r["callers"][image_of(run, s[6])] += 1
        # sub-operation decoding
        samples = rec["samples"]
        if kind == "mig-id" and num in SUBOP:
            idx, table, pred, why = SUBOP[num]
            for s in samples:
                f = s[idx] & 0xFFFFFFFF
                (r["have"] if pred(f) else r["missing"]).add(flav_name(table, f))
            r["notes"].add(why)
        if kind == "mig-id" and num not in SUBOP:
            fi = flavor_index(api[1:])
            if fi is not None:
                table = FLAVOR_TABLE_FOR.get(api[1:].split("_")[0], "")
                for s2 in samples:
                    r["have_obs"].add(flav_name(table, s2[fi] & 0xFFFFFFFF))
    # specific op decoders by api
    if api in ("_fcntl", "_fcntl$NOCANCEL", "___fcntl", "___fcntl_nocancel"):
        for s in rec["samples"]:
            c = s[1] & 0xFFFFFFFF
            sub = row("fcntl-cmd", c, FCNTL.get(c, f"F_{c}"))
            sub["native"][tgt] += 1; sub["apis"].add(api)
    if api == "___proc_info":
        for s in rec["samples"]:
            callnum, flavor = s[0] & 0xFFFFFFFF, s[2] & 0xFFFFFFFF
            ftab = PROCPID if callnum in (2, 3) else PROCSELF if callnum == 5 else {}
            sub = row("proc_info", callnum * 1000 + flavor, f"{CALLNAMES.get(callnum, 'call' + str(callnum))}/{ftab.get(flavor, 'flavor' + str(flavor))}")
            sub["native"][tgt] += 1; sub["apis"].add(api)
    if api in ("_csops", "_csops_audittoken"):
        for s in rec["samples"]:
            op = s[1] & 0xFFFFFFFF
            sub = row("csops-op", op, CSOPS.get(op, f"CS_OPS_{op}"))
            sub["native"][tgt] += 1; sub["apis"].add(api)
    if api == "_ioctl":
        for s in rec["samples"]:
            req = s[1] & 0xFFFFFFFF
            grp = chr((req >> 8) & 0xFF) if 32 <= ((req >> 8) & 0xFF) < 127 else "?"
            sub = row("ioctl-req", req, f"'{grp}' nr={req & 0xFF} len={(req >> 16) & 0x1FFF}")
            sub["native"][tgt] += 1; sub["apis"].add(api)

FLAVOR_TABLE_FOR = {"task": "mach/task_info.h", "thread": "mach/thread_info.h", "host": "mach/host_info.h",
                    "mach": "mach/vm_region.h", "vm": "mach/vm_region.h"}
def flavor_index(name):
    d = mig.get(name) or mig.get("_kernelrpc_" + name)
    if not d: return None
    for i, (pn, ty, dr) in enumerate(d["params"]):
        if any("array" in t[1] or "pointer" in t[1] for t in d["params"][:i]): return None
        if pn in ("flavor", "which", "port_type"): return i
    return None

mach_dest = {}
for tgt, rs in runs.items():
    for run in rs:
        if not run: continue
        for api, rec in run["calls"].items():
            ingest_api(tgt, run, api, rec)
        # text events
        lookups = {}
        selfp = {}
        for l in run["text"]:
            p = l.split("\t")
            kv = dict(x.split("=", 1) for x in p[2:] if "=" in x)
            if p[0] == "self": selfp = {x.split("=")[0]: int(x.split("=")[1], 16) for x in p[1:]}
            if p[0].startswith("bootstrap_look_up"): lookups[int(kv.get("port", "0x0"), 16)] = p[1]
        for l in run["text"]:
            p = l.split("\t")
            kv = dict(x.split("=", 1) for x in p[1:] if "=" in x)
            lr = int(kv["lr"], 16) if "lr" in kv else 0
            who = image_of(run, lr) if lr else "?"
            if p[0] == "sysctlbyname":
                r = row("sysctl-name", p[1], p[1]); r["native"][tgt] += 1; r["callers"][who] += 1
            elif p[0] == "sysctl":
                key = "1.14.1.<pid>" if p[1].startswith("1.14.1.") else p[1]
                r = row("sysctl-mib", key, mib_name(p[1])); r["native"][tgt] += 1; r["callers"][who] += 1
            elif p[0].startswith("bootstrap_look_up") or p[0].startswith("bootstrap_check_in") or p[0] == "xpc_connection_create_mach_service":
                r = row("bootstrap-service", p[1], p[1]); r["native"][tgt] += 1; r["callers"][who] += 1; r["apis"].add(p[0])
            elif p[0].startswith("notify_"):
                r = row("notify-name", p[1], p[1]); r["native"][tgt] += 1; r["callers"][who] += 1; r["apis"].add(p[0])
            elif p[0] == "dlopen":
                r = row("dlopen-path", p[1], p[1]); r["native"][tgt] += 1; r["callers"][who] += 1
            elif p[0] in ("mach_msg", "mach_msg2"):
                mid = int(kv["id"]) if p[0] == "mach_msg2" else int(kv["send"])
                if mid == -1: continue  # receive-only calls: the buffer id is a reply, not a request
                remote = int(kv.get("remote", "0x0"), 16)
                dest = "task" if remote == selfp.get("task") else "host" if remote == selfp.get("host") else lookups.get(remote, f"unnamed-port({who})")
                r = row("msg-id", f"{dest}:{mid}", f"id {mid} -> {dest}"); r["native"][tgt] += 1; r["callers"][who] += 1; r["apis"].add(p[0])

# ---- static tiers -------------------------------------------------------------------------
for fname, syms in imports.items():
    for s in syms:
        for kind, num, via, note in wrapper_rows(s):
            nm = bsd_name(num) if kind == "bsd" else "thread_set_tsd_base" if kind == "machdep" else TRAP_NAMES.get(num, f"trap{num}") if kind == "mach-trap" else mig_by_id.get(num, "?")
            r = row(kind, num, nm); r["static"].add(os.path.basename(fname)); r["apis"].add(s)

# ---- classify -----------------------------------------------------------------------------
BYPASS = {("bsd", 301): ("pthread_mutex", {"_pthread_mutex_lock", "_pthread_mutex_unlock"}),
          ("bsd", 302): ("pthread_mutex", {"_pthread_mutex_lock", "_pthread_mutex_unlock"}),
          ("bsd", 303): ("pthread_cond", {"_pthread_cond_wait", "_pthread_cond_signal", "_pthread_cond_broadcast"}),
          ("bsd", 304): ("pthread_cond", {"_pthread_cond_wait", "_pthread_cond_signal", "_pthread_cond_broadcast"}),
          ("bsd", 305): ("pthread_cond", {"_pthread_cond_wait", "_pthread_cond_signal", "_pthread_cond_broadcast"}),
          ("bsd", 360): ("pthread_create", {"_pthread_create"}), ("bsd", 361): ("pthread_exit", {"_pthread_exit"})}
SYSCTL_OK_MIB = {"1.2", "1.35", "1.59"}  # CTL_KERN: KERN_OSRELEASE, KERN_USRSTACK32, KERN_USRSTACK64 (process_runner.cpp:11006)
BOOT_OK = {"com.apple.windowserver.active"}
def classify(r):
    k, n = r["kind"], r["num"]
    st, eff, note = ("no", "", "")
    if k in ("bsd", "mach-trap", "mig-id", "machdep"): st, eff, note = kernel_status(k, n)
    elif k == "fcntl-cmd":
        st = "yes" if n in handled["fcntl_cmds_modelled"].values() else "no"
        eff = "" if st == "yes" else "raw fcntl falls to the unsupported-op path"
    elif k == "proc_info":
        call, flav = divmod(n, 1000)
        st = "yes" if call == 2 and flav in (13, 17) else "no"
        eff = "" if st == "yes" else "EINVAL (call!=2) or unmodeled flavor (process_runner.cpp syscall==336 handler)"
    elif k == "csops-op":
        st = "yes" if n in handled["csops_ops_modelled"] else "no"
    elif k == "sysctl-mib":
        st = "yes" if n in SYSCTL_OK_MIB else "no"
        eff = "" if st == "yes" else "EINVAL (raw __sysctl numeric MIB supports kern.usrstack*/osrelease only; process_runner.cpp syscall==202 handler)"
    elif k == "sysctl-name":
        st, eff = "yes", "reads forwarded to host; writes denied (raw_proc_syscalls.cpp case 274)"
    elif k == "bootstrap-service":
        st = "yes" if n in BOOT_OK else "no"
        if st == "yes": eff = ""
        elif "bootstrap_look_up" in r["apis"]: eff = "BOOTSTRAP_UNKNOWN_SERVICE (host-service _bootstrap_look_up, process_host.cpp case 183)"
        else: eff = "launchd/XPC message to a non-modeled port (only the plain bootstrap_look_up is intercepted)"
    elif k == "msg-id":
        st, eff = "no", "send to a non-modeled port: MACH_SEND_* error / mach-msg2-unhandled log"
    elif k == "ioctl-req":
        st, eff = "partial", "generic host forward only for 'netFdUsable' descriptors (raw_net_syscalls.inc netSyscall==54 handler)"
    elif k in ("notify-name", "dlopen-path"):
        st = "n/a"
    # host-service API intercept makes some syscall rows handled at API level
    if r["missing"] and st == "yes": st = "partial"
    byp = BYPASS.get((k, n if k == "bsd" else None)) if k == "bsd" else None
    if k == "sysctl-mib" and st == "no" and "_sysctl" in host_rt:
        byp = ("sysctl", {"_sysctl"})
    if byp and st in ("no", "stub") and byp[1] <= host_rt:
        eff = f"bypass-likely: entry {'/'.join(sorted(byp[1]))} is intercepted by cache address (host_service_registry + image_loader imports.emplace); " + ("ABORT/EINVAL only if reached by a non-intercepted entry" if k == "bsd" else "service 50 forwards any MIB to the host; EINVAL only for a raw __sysctl SVC")
        r["bypass"] = True
    else:
        r["bypass"] = False
    hs = sorted(a for a in r["apis"] if a in host_rt)
    r["status"], r["effect"], r["hs"] = st, eff, hs
    partial = ""
    if r["missing"]: partial = "missing: " + ", ".join(sorted(r["missing"]))
    elif r["have"] and n in SUBOP: partial = "all observed selectors modeled (" + ", ".join(sorted(r["have"])) + "); " + SUBOP[n][3]
    elif r["have_obs"] and st == "no": partial = "no flavor modeled; observed: " + ", ".join(sorted(r["have_obs"]))
    r["partial"] = partial
    r["note"] = note
    tg = len(r["native"])
    obs = tg > 0
    bad = st in ("no", "stub") or (st == "partial") or bool(r["missing"])
    if obs and r["bypass"]: pri = "P1" if k == "bsd" else "P2"
    elif obs and (st in ("no", "stub") or r["missing"]): pri = "P0"
    elif obs and (n in SUBOP if k == "mig-id" else False) and st in ("yes", "partial"): pri = "P1"
    elif obs and st == "partial": pri = "P1"
    elif r["static"] and st in ("no", "stub"): pri = "P2"
    elif obs: pri = "ok"
    else: pri = "P3"
    if st == "n/a": pri = "info"
    r["pri"] = pri
    abort = 3 if eff.startswith("ABORT") else 2 if (eff.startswith("EINVAL") or eff.startswith("UNVERIFIED") or "ENOTSUP" in eff or eff.startswith("BOOTSTRAP")) else 1 if eff else 0
    r["effect_rank"] = abort
    r["score"] = abort * 100000 + len(r["native"]) * 1000 + min(sum(r["native"].values()), 999)
for r in rows.values(): classify(r)

def seen(r):
    parts = []
    if r["native"]:
        parts.append("native: " + ", ".join(f"{t}x{c}" for t, c in sorted(r["native"].items())))
        if r["callers"]: parts.append("callers: " + ", ".join(f"{a}" for a, _ in r["callers"].most_common(3)))
    if r["static"]: parts.append("static: " + ", ".join(sorted(r["static"])))
    if r["apis"]: parts.append("api: " + ", ".join(sorted(r["apis"])[:3]))
    return "; ".join(parts)

def fmt_num(r):
    n = r["num"]
    if r["kind"] == "mach-trap": return f"-{n}"
    if r["kind"] == "machdep": return "0x80000000"
    return str(n)
order = {"P0": 0, "P1": 1, "P2": 2}
def group_of(r):
    k, n, nm = r["kind"], r["num"], r["name"]
    if k == "bsd" and n in (301, 302, 303, 304, 305, 360, 361): return "pthread kernel interface (psynch_*, bsdthread_create/terminate)"
    if k == "mach-trap" and n == 70: return "voucher trap -70 host_create_mach_voucher (MIG fallback 222)"
    if k in ("bootstrap-service", "msg-id"): return "launchd/XPC/notifyd/configd/cfprefsd/logd lookups and messages"
    if k == "sysctl-mib": return "numeric sysctl MIBs through raw __sysctl"
    if k == "machdep": return "thread_set_tsd_base (x16=0x80000000)"
    if k == "proc_info": return "proc_info callnum != 2 (setcontrol threadname)"
    if k == "bsd" and n in (375, 443, 530): return "kevent_id / workloop kqueue"
    if k == "bsd" and n in (447, 448, 449, 480, 481): return "multipath/batch socket calls"
    if k == "mig-id" and r["status"] == "no": return f"unmodeled kernel-object RPC {nm} ({n})"
    if k == "ioctl-req": return "ioctl on non-socket descriptors"
    return f"{k} {nm} ({n})"
ranked = sorted([r for r in rows.values() if r["pri"] in order], key=lambda r: (order[r["pri"]], -r["score"], str(r["num"])))
ok_rows = [r for r in rows.values() if r["pri"] == "ok"]
univ = collections.Counter()
for r in static_svc:
    if r["image"].endswith("libsystem_kernel.dylib") and r["num"] is not None:
        k = "mach-trap" if r["num"] < 0 else "bsd"; n = abs(r["num"])
        if (k, n) not in rows: univ[(k, n)] += 1
univ_unhandled = sorted((k, n) for (k, n) in univ if kernel_status(k, n)[0] in ("no", "stub"))

def cell(s): return str(s).replace("|", "/").replace("\n", " ")
md = []
md.append("# Service gap inventory (Azul java -version, JNI fixture, plutil -lint, tar list/extract)\n")
md.append("Generated by tools/inventory (see Reproduce). Status columns come from reading the runtime source (`handled.json`), "
          "not from running it: nothing here executed on the GPU runtime. 'native' = observed on this host by the interposer tracer; "
          "'static' = imported by the Azul closure and resolved through libsystem_kernel; rows only in the dyld-cache universe are summarised at the end.\n")
md.append("With ARM64_METAL_SURVEY=1 (added in c5ba09a) the runtime logs unhandled syscalls/traps and returns ENOSYS / KERN_INVALID_ARGUMENT instead of throwing; without it an unhandled number throws. ABORT below means the default (throwing) mode. bypass-likely: the numbers are reached through a symbol the shared-cache launcher replaces by host service (address-keyed), so the raw syscall may never be issued; this was not run.\n\nPriority: P0 observed natively and unhandled/stub/missing flavor; P1 observed, handled but partial; P2 static import only, unhandled; "
          "ranked by number of targets, then effect (ABORT > error code > soft reply), then native call count.\n")
groups = collections.OrderedDict()
for r in ranked:
    g = groups.setdefault(group_of(r), {"rows": [], "pri": r["pri"], "targets": set(), "eff": 0, "score": r["score"]})
    g["rows"].append(r); g["targets"] |= set(r["native"]); g["eff"] = max(g["eff"], r["effect_rank"] if not r["bypass"] else 0)
    g["pri"] = min(g["pri"], r["pri"]); g["score"] = max(g["score"], r["score"])
    if r["kind"] == "mig-id" and r["status"] == "partial": g["eff"] = max(g["eff"], 1)
gl = sorted(groups.items(), key=lambda kv: (kv[1]["pri"], -kv[1]["eff"], -len(kv[1]["targets"]), -kv[1]["score"]))
md.append("## Distinct gaps (rows that share one fix are grouped; the full per-row table follows)\n")
md.append("| # | gap | rows | targets | priority | worst effect |")
md.append("|---|---|---|---|---|---|")
EFF = {3: "ABORT (throws)", 2: "error code to the guest", 1: "soft reply/message error", 0: "none or bypass-likely"}
for i, (name, g) in enumerate(gl, 1):
    md.append(f"| {i} | {cell(name)} | {len(g['rows'])} | {','.join(sorted(g['targets'])) or 'static only'} | {g['pri']} | {EFF[g['eff']]} |")
md.append("")
md.append("## Per-row table\n")
md.append("| # | kind | number/id | name | seen where (static/native) | handled? | partial (flavors missing)? | priority |")
md.append("|---|---|---|---|---|---|---|---|")
for i, r in enumerate(ranked, 1):
    handled_cell = r["status"] + (f" ({r['effect']})" if r["effect"] else "") + (f"; API-intercept: {','.join(r['hs'])}" if r["hs"] else "")
    md.append(f"| {i} | {r['kind']} | {fmt_num(r)} | {cell(r['name'])} | {cell(seen(r))} | {cell(handled_cell)} | {cell(r['partial'] or (r['note'] if r['kind']=='mig-id' else ''))} | {r['pri']} |")
md.append("\n## Observed natively and handled (no action)\n")
md.append(", ".join(sorted({f"{r['kind']}:{fmt_num(r)}:{r['name']}" for r in ok_rows})) or "none")
md.append("\n## Universe-only: libsystem_kernel wrappers neither observed nor imported by the closure and unhandled\n")
md.append(f"{len(univ_unhandled)} numbers ({len([1 for k,n in univ_unhandled if k=='bsd'])} BSD, {len([1 for k,n in univ_unhandled if k=='mach-trap'])} Mach traps): "
          + ", ".join(f"{'-' if k=='mach-trap' else ''}{n}" for k, n in univ_unhandled))
open(INV + "/gaps.md.body", "w").write("\n".join(md) + "\n")
json.dump([{k: (sorted(v) if isinstance(v, set) else dict(v) if isinstance(v, collections.Counter) else v) for k, v in r.items()} for r in ranked],
          open(INV + "/gaps.json", "w"), indent=1)

audit = """
## Partial implementations (audit of what the runtime models)

All from reading the source, not from running it. Handlers are cited by id/function; exact lines are in handled.json `where`.
- task_info (MIG 3405, process_runner.cpp:11925): TASK_DYLD_INFO (17) replies KERN_NOT_SUPPORTED; every other flavor is forwarded to the runner's own task, so TASK_AUDIT_TOKEN / TASK_BASIC_INFO / TASK_VM_INFO describe the host process, not the guest. Only the 40-byte request shape on the guest task port is accepted.
- task_get_special_port (3409): selectors 1, 3, 4, 10 only. task_set_special_port (3410): selector 10 only. host_get_special_port (412): every selector replies KERN_INVALID_ARGUMENT.
- mach_port_set_attributes (3218): MACH_PORT_LIMITS_INFO only. mach_port_get_attributes is served by trap -40 instead. get/set_context (3228/3229) modeled.
- host_info (200): any flavor forwarded to the host. host_statistics/host_statistics64 (216/219) are in the exists-range: KERN_NOT_SUPPORTED. thread_info (3612), task_set_exception_ports (3413), mach_make_memory_entry_64 (3825) likewise.
- kernelObjectRoutineExists (process_runner.cpp) is only a 'this id exists' table: ids in it reply KERN_NOT_SUPPORTED, ids outside reply MIG_BAD_ID. Only the 16 ids in handled.json are modeled, each gated on destination port, message size and shape.
- proc_info (336, process_runner.cpp syscall==336 handler): callnum 2 (PIDINFO) for the caller's own pid, flavors 13 (SHORTBSDINFO) and 17 (UNIQIDENTIFIERINFO) only; any other callnum is EINVAL, a foreign pid ESRCH.
- sysctl: raw __sysctl numeric MIB (202, process_runner.cpp syscall==202 handler) supports kern.usrstack32/usrstack64/osrelease only, everything else EINVAL; sysctlbyname (274) forwards reads of any name to the host and denies writes.
- bootstrap: the host-service `_bootstrap_look_up` (process_host.cpp case 183) resolves `com.apple.windowserver.active` only; any other name is BOOTSTRAP_UNKNOWN_SERVICE. bootstrap_look_up2/3, check_in and xpc_connection_create_mach_service are not intercepted, so their launchd/XPC messages go to non-modeled ports.
- ioctl (54, raw_net_syscalls.inc netSyscall==54 handler): generic host forward for descriptors the net table owns; other descriptors are EBADF.
- fcntl (92): {n_fcntl} commands modeled ({fcntl_names}); the SDK defines {n_sdk_fcntl} F_* commands.
- csops: ops 0-17 modeled (guest_csops.hpp); other-pid operations return the kernel's errno table.
- kevent: 363/369/374 modeled; kevent_id (375), guarded_kqueue_np (443), kqueue_workloop_ctl (530) return EINVAL/ENOTSUP. Stub-only BSD numbers (heuristic, see handled.py): {stubs}.
- host_service_registry.hpp: {n_host} symbols are intercepted at symbol level ({n_host_rt} runner-eligible), applied by SharedCache::hostExportAddresses to cache exports that match; a call through such an export never reaches the raw syscall, so a row marked 'no' that is only reached through one of these symbols may be bypassed. Whether the listed pthread_*/malloc/open symbols are live for a given launch was not verified.

## What the instruments cannot see

- Raw `svc #0x80` outside the libsystem_kernel wrappers: control_svc issues 0 vs 7 raw getpid and task_self_trap calls and the tracer's _getpid count is identical (13 vs 13). Static scan shows SVC sites exist only in libsystem_kernel (530 sites) for the whole closure; none in the Azul libraries, plutil, tar or the other cache images listed.
- Calls libsystem_kernel makes to its own exports (MIG stubs calling mach_msg, `_open` calling `___open`) are not interposable. Kernel-object message ids are therefore derived from the API name through the SDK .defs (migdefs.py), not read off the wire; only mach_msg/mach_msg2 callers outside libsystem_kernel have wire ids. Trap-first stubs (mach_port_allocate, mach_vm_map, ...) are reported as the trap with the MIG id as fallback.
- dyld's own syscalls before the interposer's table is live; the libSystem-initializer window is counted only for calls through interposable stubs (___bsdthread_register shows 1, the positive control for that window).
- plutil and tar run as ad-hoc re-signed copies of the arm64e slice (SIP strips DYLD_* from the /usr/bin originals); platform-binary behaviour (csops flags, entitlement checks) can differ from the originals.
- Caller attribution is the immediate caller of the interposed wrapper (x30), not the root cause. First 256 calls per function are sampled (counts are exact), so flavors/commands that first appear later are missed.
- dtrace/dtruss: `dtrace: failed to initialize dtrace: DTrace requires additional privileges` (SIP on, sudo needs a password, Developer mode off so lldb attach is out). Not attempted further.
- Static op values (fcntl cmd, csops op, proc_info flavor, sysctl name) at Azul call sites were not recovered; only observed values exist. Static tier covers libsystem_kernel-exported imports; libc imports are not expanded to syscalls. Static service strings are in strings.txt.
- Handled-ness is read from source text; nothing was run on the GPU runtime. Stub detection is a heuristic over the handler's terminal statement.

## Reproduce

```
cd /Users/ender/code/evamcep/arm64-metal-runtime/wt-trace
tools/inventory/inventory.sh "$PWD"        # whole pipeline, ~2 min, no GPU, no sudo
# individual steps (WORK=/Users/ender/code/evamcep/arm64-metal-work):
python3 -I tools/inventory/handled.py "$PWD" $WORK/inventory/handled.json
python3 -I tools/inventory/static_scan.py $WORK/inventory/dsc $WORK/inventory/static_svc.json <Mach-O files...>
tools/inventory/run_all.sh "$PWD" $WORK/logs/trace                 # controls + java -version, jni-entry, plutil -lint, tar -tvf/-xf under the tracer
python3 -I tools/inventory/gaps.py $WORK/logs/trace <runs-dir> $WORK/inventory
```
Raw traces: logs/trace/runs-<epoch>/<target>/raw.<pid>(.json). Positive controls are run_all.sh's `control` (getpid/open/fcntl/task_info/sysctlbyname/bootstrap/mach_msg id 7777, all must appear) and `control-svc-0/7`.
"""
fn = handled["fcntl_cmds_modelled"]
sdk_fc = [n for n in FCNTL.values()]
stubs = sorted(int(k) for k, v in bsd_h.items() if v["stub_sites"] and not v["other_sites"])
audit = audit.format(n_fcntl=len(fn), fcntl_names=", ".join(sorted(fn)), n_sdk_fcntl=len(set(sdk_fc)), stubs=", ".join(f"{n} {bsd_name(n)}" for n in stubs),
                     n_host=len(handled["host_services"]), n_host_rt=len(host_rt))
open(INV + "/gaps.md", "w").write("\n".join(md) + "\n" + audit)
print(len(ranked), "ranked rows;", len(ok_rows), "ok;", len(univ_unhandled), "universe-unhandled")
