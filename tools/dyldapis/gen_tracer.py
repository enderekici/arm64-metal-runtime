#!/usr/bin/env python3
"""Generate a DYLD_INSERT_LIBRARIES tracer that counts calls to the given libdyld exports.
usage: gen_tracer.py NAMES_FILE OUT_DIR   (one exported symbol per line, leading '_')
Writes OUT_DIR/stubs.S and OUT_DIR/tracer.c; build with build_tracer.sh."""
import sys
names = [l.strip() for l in open(sys.argv[1]) if l.strip()]
out = sys.argv[2]
s = [".arch armv8.1-a", ".text", ".align 2"]
tab = [".section __DATA,__interpose"]
for i, n in enumerate(names):
    s += [f"_dt_stub_{i}:",
          "  adrp x9, _g_dt_counts@PAGE", "  add x9, x9, _g_dt_counts@PAGEOFF",
          f"  add x9, x9, #{8 * i}", "  mov x10, #1", "  ldaddal x10, x11, [x9]", "  cbnz x11, 9f",
          "  adrp x9, _g_dt_first@PAGE", "  add x9, x9, _g_dt_first@PAGEOFF",
          f"  str x30, [x9, #{8 * i}]", f"9: b {n}"]
    tab += ["  .align 3", f"  .quad _dt_stub_{i}", f"  .quad {n}"]
open(out + "/stubs.S", "w").write("\n".join(s + tab) + "\n")
c = ['#include <dlfcn.h>', '#include <stdio.h>', '#include <stdlib.h>', '#include <stdint.h>',
     '#include <unistd.h>',
     f'unsigned long g_dt_counts[{len(names)}];', f'unsigned long g_dt_first[{len(names)}];',
     'static const char* const names[] = {' + ",".join(f'"{n}"' for n in names) + '};',
     '__attribute__((destructor)) static void dt_dump(void) {',
     '  const char* out = getenv("DT_OUT"); if (!out) return;',
     '  char path[1024]; snprintf(path, sizeof path, "%s.%d", out, getpid());',
     '  FILE* f = fopen(path, "w"); if (!f) return;',
     f'  for (int i = 0; i < {len(names)}; ++i) if (g_dt_counts[i]) {{',
     '    Dl_info info = {0}; const char* img = "?"; unsigned long off = 0;',
     '    if (dladdr((void*)g_dt_first[i], &info) && info.dli_fname) { img = info.dli_fname; off = g_dt_first[i] - (unsigned long)info.dli_fbase; }',
     '    fprintf(f, "%s %lu %s+0x%lx\\n", names[i] + 1, g_dt_counts[i], img, off); }',
     '  fclose(f); }']
open(out + "/tracer.c", "w").write("\n".join(c) + "\n")
print(len(names), "stubs")
