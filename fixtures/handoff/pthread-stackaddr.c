#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>

int main(void) {
  int mib[] = {CTL_KERN, KERN_USRSTACK64};
  uintptr_t sysctl_stack = 0;
  size_t length = sizeof(sysctl_stack);
  const int result = sysctl(mib, 2, &sysctl_stack, &length, 0, 0);
  const uintptr_t pthread_stack =
      (uintptr_t)pthread_get_stackaddr_np(pthread_self());
  printf("pthread-stackaddr=%s size=%zu\n",
         result == 0 && length == sizeof(sysctl_stack) &&
                 pthread_stack == sysctl_stack ? "matches-sysctl" : "mismatch",
         length);
  return result == 0 && length == sizeof(sysctl_stack) &&
                 pthread_stack == sysctl_stack ? 0 : 1;
}
