// Blindness control: issues getpid(20) and task_self_trap(-28) by raw SVC argv[1] times; the tracer's
// _getpid/_task_self_trap counts must be identical for argv[1]=0 and argv[1]=7 (raw SVC is invisible).
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 0;
  for (int i = 0; i < n; i++) {
    register long x16 __asm__("x16") = 20; register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
    register long y16 __asm__("x16") = -28; register long y0 __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(y0) : "r"(y16) : "memory", "cc");
  }
  puts("svc control done");
  return 0;
}
