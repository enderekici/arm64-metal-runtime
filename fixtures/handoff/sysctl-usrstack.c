#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

struct raw_result {
  uint64_t value;
  uint64_t nzcv;
};

static struct raw_result raw_sysctl(int *mib, uint32_t mib_count, void *old_value,
                                    size_t *old_length, void *new_value,
                                    size_t new_length) {
  register uint64_t x0 __asm__("x0") = (uint64_t)mib;
  register uint64_t x1 __asm__("x1") = mib_count;
  register uint64_t x2 __asm__("x2") = (uint64_t)old_value;
  register uint64_t x3 __asm__("x3") = (uint64_t)old_length;
  register uint64_t x4 __asm__("x4") = (uint64_t)new_value;
  register uint64_t x5 __asm__("x5") = new_length;
  register uint64_t x16 __asm__("x16") = SYS_sysctl;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %6, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
                     "+r"(x4), "+r"(x5), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, nzcv};
}

int main(void) {
  int stack64_mib[] = {CTL_KERN, KERN_USRSTACK64};
  int stack32_mib[] = {CTL_KERN, KERN_USRSTACK32};
  uint64_t raw_stack64 = 0;
  uint64_t named_stack64 = 0;
  uint32_t raw_stack32 = 0;
  uint32_t named_stack32 = 0;
  size_t raw64_length = sizeof(raw_stack64);
  size_t named64_length = sizeof(named_stack64);
  size_t raw32_length = sizeof(raw_stack32);
  size_t named32_length = sizeof(named_stack32);
  struct raw_result raw64 = raw_sysctl(stack64_mib, 2, &raw_stack64,
                                       &raw64_length, 0, 0);
  int named64 = sysctlbyname("kern.usrstack64", &named_stack64,
                             &named64_length, 0, 0);
  struct raw_result raw32 = raw_sysctl(stack32_mib, 2, &raw_stack32,
                                       &raw32_length, 0, 0);
  int named32 = sysctlbyname("kern.usrstack", &named_stack32,
                             &named32_length, 0, 0);
  size_t query_length = 0;
  const int query_result = sysctlbyname("kern.usrstack64", 0,
                                        &query_length, 0, 0);
  size_t named_short_length = sizeof(uint32_t);
  errno = 0;
  const int named_short_result = sysctlbyname("kern.usrstack64", &named_stack64,
                                               &named_short_length, 0, 0);
  const int named_short_errno = errno;
  size_t raw_short_length = sizeof(uint32_t);
  errno = 0;
  const struct raw_result raw_short = raw_sysctl(stack64_mib, 2, &raw_stack64,
                                                  &raw_short_length, 0, 0);
  const int raw_short_errno = errno;
  uintptr_t current_sp = 0;
  __asm__ volatile("mov %0, sp" : "=r"(current_sp));
  const int stack_range_valid = raw_stack64 >= current_sp &&
      raw_stack64 - current_sp <= 16U * 1024U * 1024U;
  const int result_valid = raw64.value == 0 && ((raw64.nzcv >> 29) & 1) == 0 &&
      raw64_length == sizeof(raw_stack64) && named64 == 0 &&
      named64_length == sizeof(named_stack64) && raw_stack64 == named_stack64 &&
      raw32.value == 0 && ((raw32.nzcv >> 29) & 1) == 0 &&
      raw32_length == sizeof(raw_stack32) && named32 == 0 &&
      named32_length == sizeof(named_stack32) && raw_stack32 == named_stack32 &&
      raw_stack32 == (uint32_t)raw_stack64 && query_result == 0 &&
      query_length == sizeof(raw_stack64) && named_short_result == -1 &&
      named_short_errno == ERANGE && named_short_length == sizeof(uint32_t) &&
      raw_short.value == ERANGE && ((raw_short.nzcv >> 29) & 1) == 1 &&
      raw_short_length == sizeof(uint32_t) && raw_short_errno == 0;
  printf("usrstack64=%s usrstack32=%s query=%s short=%s range=%s\n",
         raw_stack64 == named_stack64 ? "match" : "mismatch",
         raw_stack32 == named_stack32 ? "match" : "mismatch",
         query_result == 0 && query_length == sizeof(raw_stack64) ? "ok" : "bad",
         named_short_result == -1 && raw_short.value == ERANGE ? "ok" : "bad",
         stack_range_valid ? "near-sp" : "bad");
  return result_valid && stack_range_valid ? 0 : 1;
}
