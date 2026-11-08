#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>

struct result { uint64_t value; uint64_t nzcv; };
static struct result raw_sysctl(int *mib, uint32_t miblen, void *old,
                                size_t *oldlen, void *newp, size_t newlen) {
  register uint64_t x0 __asm__("x0") = (uint64_t)mib;
  register uint64_t x1 __asm__("x1") = miblen;
  register uint64_t x2 __asm__("x2") = (uint64_t)old;
  register uint64_t x3 __asm__("x3") = (uint64_t)oldlen;
  register uint64_t x4 __asm__("x4") = (uint64_t)newp;
  register uint64_t x5 __asm__("x5") = newlen;
  register uint64_t x16 __asm__("x16") = SYS_sysctl;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %6, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
                     "+r"(x4), "+r"(x5), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct result){x0, nzcv};
}
int main(void) {
  int mib[] = { CTL_KERN, KERN_OSRELEASE };
  char value[256] = {0}; size_t len = sizeof(value);
  struct result good = raw_sysctl(mib, 2, value, &len, 0, 0);
  printf("ok value=%llu carry=%llu len=%zu release=%s errno=%d\n",
         (unsigned long long)good.value, (unsigned long long)((good.nzcv >> 29) & 1),
         len, value, errno);
  size_t query_len = 0;
  struct result query = raw_sysctl(mib, 2, 0, &query_len, 0, 0);
  printf("query value=%llu carry=%llu len=%zu\n",
         (unsigned long long)query.value,
         (unsigned long long)((query.nzcv >> 29) & 1), query_len);
  char short_value[2] = {0}; size_t short_len = sizeof(short_value);
  struct result short_result = raw_sysctl(mib, 2, short_value, &short_len, 0, 0);
  printf("short value=%llu carry=%llu len=%zu data=%d,%d\n",
         (unsigned long long)short_result.value,
         (unsigned long long)((short_result.nzcv >> 29) & 1), short_len,
         (unsigned char)short_value[0], (unsigned char)short_value[1]);
  struct result nullable_len = raw_sysctl(mib, 2, 0, 0, 0, 0);
  printf("nullable-len value=%llu carry=%llu\n",
         (unsigned long long)nullable_len.value,
         (unsigned long long)((nullable_len.nzcv >> 29) & 1));
  struct result missing_len = raw_sysctl(mib, 2, value, 0, 0, 0);
  printf("missing-len value=%llu carry=%llu\n",
         (unsigned long long)missing_len.value,
         (unsigned long long)((missing_len.nzcv >> 29) & 1));
  int long_mib[CTL_MAXNAME + 1] = {CTL_KERN, KERN_OSRELEASE};
  size_t long_len = 17;
  struct result too_many = raw_sysctl(long_mib, CTL_MAXNAME + 1, value, &long_len, 0, 0);
  printf("too-many value=%llu carry=%llu len=%zu\n",
         (unsigned long long)too_many.value,
         (unsigned long long)((too_many.nzcv >> 29) & 1), long_len);
  return good.value != 0 || ((good.nzcv >> 29) & 1) != 0 ||
         query.value != 0 || ((query.nzcv >> 29) & 1) != 0 || query_len != 7 ||
         short_result.value != ENOMEM || ((short_result.nzcv >> 29) & 1) == 0 ||
         short_len != 0 || nullable_len.value != 0 ||
         ((nullable_len.nzcv >> 29) & 1) != 0 || missing_len.value != ENOMEM ||
         ((missing_len.nzcv >> 29) & 1) == 0 || too_many.value != EINVAL ||
         ((too_many.nzcv >> 29) & 1) == 0 || long_len != 17;
}
