#!/usr/bin/env python3
"""Generate a native-vs-arm64-metal differential for AdvSIMD encodings.

usage: gen-neon-differential.py <class> <out.c>
Enumerates encodings of one class (fixed registers d=v0, n=v1, m=v2), keeps those the
assembler/disassembler accepts, and emits a C harness that runs each one on random and
edge-case vectors and prints "<disassembly> <hash>" per encoding.
Run: <bin> [trials] [start] [count] [dump]   (dump prints per-trial values)
"""
import subprocess, sys, tempfile, os, re

def three_same():
    for q in (0, 1):
        for u in (0, 1):
            for size in range(4):
                for opc in range(0x18):
                    if opc == 3: continue
                    yield 0x0e200400 | q << 30 | u << 29 | size << 22 | 2 << 16 | opc << 11 | 1 << 5
def three_diff():
    for q in (0, 1):
        for u in (0, 1):
            for size in range(3):
                for opc in range(15):
                    yield 0x0e200000 | q << 30 | u << 29 | size << 22 | 2 << 16 | opc << 12 | 1 << 5
def misc():
    for q in (0, 1):
        for u in (0, 1):
            for size in range(4):
                for opc in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x12, 0x13, 0x14, 0x16, 0x17):
                    if opc in (0x16, 0x17) and (u or size > 1): continue  # fcvtxn, bfcvtn: not lowered
                    yield 0x0e200800 | q << 30 | u << 29 | size << 22 | opc << 12 | 1 << 5
def permute():
    for q in (0, 1):
        for size in range(4):
            for opc in (1, 2, 3, 5, 6, 7):
                yield 0x0e000800 | q << 30 | size << 22 | 2 << 16 | opc << 12 | 1 << 5
def shift_imm():
    for q in (0, 1):
        for u in (0, 1):
            for immh in (1, 2, 4, 8, 3, 5, 9, 15):
                for immb in (0, 1, 3, 5, 7):
                    for opc in (0, 2, 4, 6, 8, 0xa, 0xc, 0xe, 0x10, 0x11, 0x12, 0x13, 0x14):
                        yield 0x0f000400 | q << 30 | u << 29 | immh << 19 | immb << 16 | opc << 11 | 1 << 5
def by_element():
    for q in (0, 1):
        for u in (0, 1):
            for size in (1, 2):
                for opc in ((0, 2, 4, 6, 0xa) if u else (2, 3, 6, 7, 8, 0xa, 0xb, 0xc, 0xd)):
                    for hlm in range(8):
                        h, l, m = hlm >> 2 & 1, hlm >> 1 & 1, hlm & 1
                        if size == 2 and m: continue  # Rm would be v18, outside the harness inputs
                        yield (0x0f000000 | q << 30 | u << 29 | size << 22 | l << 21 | m << 20 | 2 << 16
                               | opc << 12 | h << 11 | 1 << 5)
def modified_imm():
    for q in (0, 1):
        for op in (0, 1):
            for cmode in range(15):
                for imm8 in (0x00, 0x5a, 0xff, 0x80, 0x01, 0xa5):
                    yield (0x0f000400 | q << 30 | op << 29 | (imm8 >> 5) << 16 | cmode << 12 | (imm8 & 31) << 5)

def fp_misc():
    for base in (0x0e200800, 0x5e200800):
        for u in (0, 1):
            for size in range(4):
                for opc in range(0x0c, 0x20):
                    for q in ((0, 1) if base == 0x0e200800 else (0,)):
                        yield base | q << 30 | u << 29 | size << 22 | opc << 12 | 1 << 5
def fp_misc16():
    for base in (0x0e780800, 0x5e780800):
        for u in (0, 1):
            for a in (0, 1):
                for opc in range(0x0c, 0x20):
                    for q in ((0, 1) if base == 0x0e780800 else (0,)):
                        yield base | q << 30 | u << 29 | a << 23 | opc << 12 | 1 << 5
def fp_three():
    for base in (0x0e200400, 0x5e200400):
        for u in (0, 1):
            for size in range(4):
                for opc in range(0x18, 0x20):
                    for q in ((0, 1) if base == 0x0e200400 else (0,)):
                        yield base | q << 30 | u << 29 | size << 22 | 2 << 16 | opc << 11 | 1 << 5
def fp_three16():
    for base in (0x0e400400, 0x5e400400):
        for u in (0, 1):
            for a in (0, 1):
                for opc in range(0, 8):
                    for q in ((0, 1) if base == 0x0e400400 else (0,)):
                        yield base | q << 30 | u << 29 | a << 23 | 2 << 16 | opc << 11 | 1 << 5
def fp_pair():
    for u in (0, 1):
        for size in range(4):
            for opc in range(0x0c, 0x10):
                yield 0x5e300800 | u << 29 | size << 22 | opc << 12 | 1 << 5
    for u in (0, 1):
        for a in (0, 1):
            for opc in range(0x0c, 0x10):
                yield 0x5e300800 | u << 29 | a << 23 | opc << 12 | 1 << 5
def fixed_fp():
    for base in (0x0f000400, 0x5f000400):
        for u in (0, 1):
            for immh in (2, 3, 4, 5, 8, 9, 15):
                for immb in (0, 1, 3, 7):
                    for opc in (0x1c, 0x1f):
                        for q in ((0, 1) if base == 0x0f000400 else (0,)):
                            yield base | q << 30 | u << 29 | immh << 19 | immb << 16 | opc << 11 | 1 << 5
def scalar_int():
    for u in (0, 1):
        for size in range(4):
            for opc in range(0x18):
                yield 0x5e200400 | u << 29 | size << 22 | 2 << 16 | opc << 11 | 1 << 5
            for opc in range(0x18):
                yield 0x5e200800 | u << 29 | size << 22 | opc << 12 | 1 << 5
            for opc in range(15):
                yield 0x5e200000 | u << 29 | size << 22 | 2 << 16 | opc << 12 | 1 << 5
        for immh in (1, 2, 4, 8, 15):
            for immb in (0, 1, 3, 7):
                for opc in (0, 2, 4, 6, 8, 0xa, 0xc, 0xe, 0x10, 0x11, 0x12, 0x13, 0x14):
                    yield 0x5f000400 | u << 29 | immh << 19 | immb << 16 | opc << 11 | 1 << 5
CLASSES = {"fp_misc": fp_misc, "fp_misc16": fp_misc16, "fp_three": fp_three, "fp_three16": fp_three16, "fp_pair": fp_pair, "fixed_fp": fixed_fp, "scalar_int": scalar_int, "modified_imm": modified_imm, "three_same": three_same, "three_diff": three_diff, "misc": misc, "permute": permute,
           "shift_imm": shift_imm, "by_element": by_element}

def disassemble(words):
    d = tempfile.mkdtemp()
    with open(d + "/a.s", "w") as f:
        for w in words: f.write(f".inst 0x{w:08x}\n")
    subprocess.check_call(["clang", "-arch", "arm64", "-c", d + "/a.s", "-o", d + "/a.o"])
    out = subprocess.check_output(["xcrun", "llvm-objdump", "-d", "--no-show-raw-insn", d + "/a.o"], text=True)
    lines = [re.sub(r"^\s*[0-9a-f]+:\s*", "", l) for l in out.splitlines() if re.match(r"^\s+[0-9a-f]+:", l)]
    return [re.sub(r"\s+", " ", l.strip()) for l in lines]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t st = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static uint64_t lane_special(int w) {
  uint64_t mask = w == 64 ? ~0ull : (1ull << w) - 1, sign = 1ull << (w - 1);
  switch (rnd() % 10) {
  case 0: return 0; case 1: return 1; case 2: return mask; case 3: return sign; case 4: return sign - 1;
  case 5: return (sign + 1) & mask; case 6: return (uint64_t)(int64_t)((int)(rnd() % 141) - 70) & mask;
  default: return rnd() & mask;
  }
}
static void pick(uint64_t v[2]) {
  int mode = rnd() % 4;
  if (mode == 0) { v[0] = rnd(); v[1] = rnd(); return; }
  int w = 8 << (rnd() % 4);
  v[0] = v[1] = 0;
  for (int i = 0; i < 128 / w; i++) {
    uint64_t x = lane_special(w); int bit = i * w;
    v[bit >> 6] |= x << (bit & 63);
  }
}
@TESTS@
typedef void (*test_fn)(const uint64_t *, uint64_t *);
static const struct { const char *tag; test_fn fn; } tests[] = {
@TABLE@
};
int main(int argc, char **argv) {
  int n = 40, start = 0, count = (int)(sizeof tests / sizeof tests[0]), dump = argc > 4;
  if (argc > 1) { n = 0; for (char *p = argv[1]; *p; p++) n = n * 10 + (*p - '0'); }
  if (argc > 2) { start = 0; for (char *p = argv[2]; *p; p++) start = start * 10 + (*p - '0'); }
  if (argc > 3) { count = 0; for (char *p = argv[3]; *p; p++) count = count * 10 + (*p - '0'); }
  for (int t = start; t < start + count && t < (int)(sizeof tests / sizeof tests[0]); t++) {
    uint64_t h = 1469598103934665603ull; st = 0x9e3779b97f4a7c15ull + t;
    for (int i = 0; i < n; i++) {
      uint64_t in[6], out[3] __attribute__((aligned(16)));
      pick(in); pick(in + 2); pick(in + 4);
      if (rnd() % 8 == 0) { in[2] = in[0]; in[3] = in[1]; }
      tests[t].fn(in, out);
      for (int k = 0; k < 3; k++) { h ^= out[k]; h *= 1099511628211ull; }
      if (dump) printf("%s %016llx%016llx %016llx %016llx%016llx %016llx%016llx -> %016llx%016llx %llx\n", tests[t].tag,
        (unsigned long long)in[1], (unsigned long long)in[0], 0ull, (unsigned long long)in[3], (unsigned long long)in[2],
        (unsigned long long)in[5], (unsigned long long)in[4], (unsigned long long)out[1], (unsigned long long)out[0], (unsigned long long)out[2]);
    }
    printf("%d %s %016llx\n", t, tests[t].tag, (unsigned long long)h);
  }
  return 0;
}
'''

def main():
    cls, out = sys.argv[1], sys.argv[2]
    words = sorted(set(CLASSES[cls]()))
    dis = disassemble(words)
    assert len(dis) == len(words), (len(dis), len(words))
    good = [(w, t) for w, t in zip(words, dis) if not (t.startswith(".") or "unknown" in t or t.startswith("udf")
            or re.match(r"(fadd|fsub|fmul|fmla|fmls|fabs|fneg|fmlal|fmlsl)[. ]", t))]
    ex = os.environ.get("EXCLUDE")
    if ex:
        good = [(w, t) for w, t in good if not re.search(ex, t)]
    tests, table = [], []
    for i, (w, t) in enumerate(good):
        tests.append(f'''static void t{i}(const uint64_t *in, uint64_t *out) {{
  __asm__ volatile("ldr q0, [%0]\\n ldr q1, [%0, #16]\\n ldr q2, [%0, #32]\\n msr fpsr, xzr\\n .inst 0x{w:08x}\\n mrs x9, fpsr\\n str q0, [%1]\\n str x9, [%1, #16]\\n"
                   :: "r"(in), "r"(out) : "x9", "v0", "v1", "v2", "memory"); }}''')
        table.append(f'  {{"{t.replace(chr(34), "")}", t{i}}},')
    def write(keep):
        open(out, "w").write(HARNESS.replace("@TESTS@", "\n".join(tests[i] for i in keep))
                             .replace("@TABLE@", "\n".join(table[i] for i in keep)))
    keep = list(range(len(good)))
    write(keep)
    # Drop encodings the host CPU rejects even though the assembler accepts them.
    probe = out + ".probe"
    subprocess.check_call(["clang", "-O1", "-arch", "arm64", out, "-o", probe])
    alive = [i for i, k in enumerate(keep)
             if subprocess.run([probe, "1", str(i), "1"], stdout=subprocess.DEVNULL).returncode == 0]
    # Renumber: tests are indexed by position, so re-emit with only the survivors.
    keep = alive
    write(keep)
    os.remove(probe)
    print(f"{cls}: {len(keep)} runnable of {len(good)} valid of {len(words)} candidates", file=sys.stderr)

main()
