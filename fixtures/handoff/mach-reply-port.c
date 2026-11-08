#include <mach/mach_init.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t raw_trap(uint64_t number, uint64_t arg0, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x2 __asm__("x2") = arg2;
  register uint64_t x3 __asm__("x3") = arg3;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x16) : "memory");
  return x0;
}

int main(void) {
  const uint64_t task = mach_task_self();
  const uint64_t first = raw_trap((uint64_t)-26, 0, 0, 0, 0);
  const uint64_t second = raw_trap((uint64_t)-26, 0, 0, 0, 0);
  const uint64_t wrong_task = raw_trap((uint64_t)-25, 0, first, 0, 0);
  const uint64_t destroy_first = raw_trap((uint64_t)-25, task, first, 0, 0);
  const uint64_t destroy_second = raw_trap((uint64_t)-25, task, second, 0, 0);
  const uint64_t destroy_first_again = raw_trap((uint64_t)-25, task, first, 0, 0);

  printf("task=%d first=%d second=%d distinct=%d destroy_first=%llu "
         "destroy_second=%llu destroy_again=%llu wrong_task=%llu\n",
         task != 0, first != 0, second != 0, first != second,
         (unsigned long long)destroy_first, (unsigned long long)destroy_second,
         (unsigned long long)destroy_first_again, (unsigned long long)wrong_task);

  return !task || !first || !second || first == second ||
         destroy_first != 0 || destroy_second != 0 ||
         destroy_first_again != 15 || wrong_task != 0x10000003;
}
