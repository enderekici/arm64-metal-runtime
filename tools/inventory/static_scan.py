#!/usr/bin/env python3
"""Static SVC #0x80 inventory. usage: static_scan.py DSC_DIR OUT.json FILE...
DSC_DIR holds dsc-dump output (<name>.bin/.idx); FILE... are on-disk Mach-O (arm64 or arm64e slice).
For each SVC finds the preceding write to x16 (MOVZ/MOVN/MOVK) in the same straight-line run and the
nearest exported symbol. Records num=None when x16 is not set by an immediate (dynamic: syscall(2))."""
import glob, json, os, struct, sys, bisect

SVC = 0xD4001001

def x16_write(w):
    """(kind, imm16, hw, sf) if w writes x16/w16 by MOVZ/MOVN/MOVK."""
    if w & 0x1F != 16: return None
    top = w & 0x7F800000
    sf = w >> 31
    imm = (w >> 5) & 0xFFFF
    hw = (w >> 21) & 3
    return {0x52800000: "z", 0x12800000: "n", 0x72800000: "k"}.get(top) and (
        {0x52800000: "z", 0x12800000: "n", 0x72800000: "k"}[top], imm, hw, sf)

def is_break(w):
    # RET, BR, B, BL, BLR, CBZ-family, B.cond, TBZ, ERET: end of straight-line region
    if w in (0xD65F03C0,): return True
    if w >> 26 in (0b000101, 0b100101): return True
    if (w & 0xFE000000) == 0xD6000000: return True
    if (w & 0x7E000000) == 0x34000000: return True
    if (w & 0xFF000010) == 0x54000000: return True
    if (w & 0x7E000000) == 0x36000000: return True
    return False

def scan_text(data, base, syms_sorted, sym_addr, image, out, lookback=24):
    words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
    for i, w in enumerate(words):
        if w != SVC: continue
        val, have = 0, False
        known = None
        j = i - 1
        cnt = 0
        movs = []
        while j >= 0 and cnt < lookback:
            pw = words[j]
            if is_break(pw): break
            wr = x16_write(pw)
            if wr: movs.append(wr)
            elif (pw & 0xFFE0FFFF) == 0xAA0003F0 or (pw & 0x7FE0FFE0) == 0x2A0003E0 and (pw & 0x1F) == 16:
                movs.append(("reg", 0, 0, 0))
            j -= 1; cnt += 1
        # movs are youngest-first; the youngest z/n/reg starts the value, k's after it (older) do not apply
        num = None
        for idx, m in enumerate(movs):
            if m[0] in ("z", "n"):
                kind, imm, hw, sf = m
                v = imm << (16 * hw)
                if kind == "n": v = ~v
                # movk written later than movz (i.e. younger) already seen at lower idx
                for k in reversed(movs[:idx]):
                    if k[0] == "k":
                        _, kim, khw, _ = k
                        v = (v & ~(0xFFFF << (16 * khw))) | (kim << (16 * khw))
                bits = 64 if sf else 32
                v &= (1 << bits) - 1
                num = v - (1 << bits) if v >> (bits - 1) else v
                break
            if m[0] == "reg": break
        addr = base + 4 * i
        k = bisect.bisect_right(syms_sorted, addr) - 1
        sym = sym_addr[syms_sorted[k]] if k >= 0 else None
        out.append({"image": image, "addr": addr, "sym": sym, "num": num})

def load_dsc(d):
    res = []
    for idx in sorted(glob.glob(d + "/*.idx")):
        name = None; texts = []; exports = {}
        for l in open(idx):
            p = l.rstrip("\n").split(" ", 3)
            if p[0] == "image": name = l.split(" ", 1)[1].strip()
            elif p[0] == "text": texts.append((int(p[1]), int(p[2]), int(p[3])))
            elif p[0] == "export":
                a = int(p[1]); n = p[2]
                if any(t[0] <= a < t[0] + t[2] for t in texts) or True: exports.setdefault(a, n)
        res.append((name, idx[:-4] + ".bin", texts, exports))
    return res

def macho_slice(path):
    d = open(path, "rb").read()
    magic = struct.unpack(">I", d[:4])[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        n = struct.unpack(">I", d[4:8])[0]
        for i in range(n):
            if magic == 0xCAFEBABE:
                cpu, sub, off, size, al = struct.unpack(">iiIII", d[8 + 20 * i: 28 + 20 * i])
            else:
                cpu, sub, off, size, al, _ = struct.unpack(">iiQQII", d[8 + 32 * i: 40 + 32 * i])
            if cpu == 0x0100000C and (sub & 0xFFFFFF) in (0, 2):  # arm64 / arm64e (pick first arm64e or arm64)
                return d[off: off + size]
        raise SystemExit("no arm64 slice in " + path)
    return d

def macho_info(d):
    magic, cpu, sub, ft, ncmds, sz, flags, _ = struct.unpack("<IiiIIIII", d[:32])
    off = 32
    texts, symtab, segs = [], None, []
    for _ in range(ncmds):
        cmd, size = struct.unpack("<II", d[off: off + 8])
        if cmd == 0x19:
            segname = d[off + 8: off + 24].split(b"\0")[0].decode()
            vmaddr, vmsize, fo, fs, mp, ip, nsects, fl = struct.unpack("<QQQQiiII", d[off + 24: off + 72])
            segs.append((segname, vmaddr, vmsize))
            so = off + 72
            for s in range(nsects):
                sn = d[so: so + 16].split(b"\0")[0].decode(); sg = d[so + 16: so + 32].split(b"\0")[0].decode()
                addr, ssz, soff = struct.unpack("<QQI", d[so + 32: so + 52])
                if sg == "__TEXT" and sn == "__text": texts.append((addr, soff, ssz))
                so += 80
        elif cmd == 0x2:
            symtab = struct.unpack("<IIII", d[off + 8: off + 24])
        off += size
    syms = {}
    if symtab:
        symoff, nsyms, stroff, strsize = symtab
        for i in range(nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack("<IBBHQ", d[symoff + 16 * i: symoff + 16 * i + 16])
            if n_type & 0xE0 or (n_type & 0x0E) != 0x0E or n_sect == 0: continue
            e = d.index(b"\0", stroff + n_strx)
            syms.setdefault(n_value, d[stroff + n_strx: e].decode(errors="replace"))
    return texts, syms

def main():
    dsc_dir, outp, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    out = []
    for name, binp, texts, exports in load_dsc(dsc_dir):
        data = open(binp, "rb").read()
        addrs = sorted(exports)
        for (start, off, size) in texts:
            scan_text(data[off: off + size], start, addrs, exports, name, out)
    for f in files:
        d = macho_slice(f)
        texts, syms = macho_info(d)
        addrs = sorted(syms)
        for (addr, off, size) in texts:
            scan_text(d[off: off + size], addr, addrs, syms, f, out)
    json.dump(out, open(outp, "w"))
    by = {}
    for r in out: by.setdefault(r["image"], []).append(r)
    for k, v in sorted(by.items()): print(len(v), k)

main()
