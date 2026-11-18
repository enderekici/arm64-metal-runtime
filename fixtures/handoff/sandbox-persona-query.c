#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Read-only Sandbox policy query made by libsystem_trace during AppKit startup.
struct sandbox_query {
  uint64_t *reply;
  int64_t pid;
  const char *operation;
  uint64_t filter;
  uintptr_t argument;
  uint64_t flags;
};

int main(int argc, char **argv) {
  uint64_t reply[2] = {UINT64_MAX, UINT64_MAX};
  struct sandbox_query query = {reply, getpid(), "syscall-unix", 65, SYS_persona, 0};
  const char *mode = argc > 1 ? argv[1] : "persona";
  if (strcmp(mode, "other-number") == 0) query.argument = SYS_getpid;
  else if (strcmp(mode, "other-operation") == 0) query.operation = "file-read-data";
  else if (strcmp(mode, "other-subject") == 0) query.pid = 1;
  else if (strcmp(mode, "other-flags") == 0) query.flags = 1;
  else if (strcmp(mode, "null-reply") == 0) query.reply = NULL;
  else if (strcmp(mode, "path") == 0) {
    query.operation = "file-read-data";
    query.filter = 1;
    query.argument = (uintptr_t)"/dev/null";
  } else if (strcmp(mode, "persona") != 0) return 2;

  register uintptr_t x0 __asm__("x0") = (uintptr_t)"Sandbox";
  register uintptr_t x1 __asm__("x1") = 2;
  register uintptr_t x2 __asm__("x2") = (uintptr_t)&query;
  register uintptr_t x16 __asm__("x16") = SYS___mac_syscall;
  uint32_t carry;
  __asm__ volatile("svc #0x80\n\tcset %w[carry], cs"
                   : [carry] "=&r"(carry), "+r"(x0), "+r"(x1), "+r"(x2)
                   : "r"(x16) : "cc", "memory");
  if (reply[1] != UINT64_MAX) return 3;
  if (!query.reply) {
    if (carry != 1 || x0 != EFAULT || reply[0] != UINT64_MAX) return 4;
    puts("null-reply=EFAULT");
  } else {
    if (carry != 0 || x0 != 0 || reply[0] > 1) return 5;
    printf("decision=%" PRIu64 "\n", reply[0]);
  }
  return 0;
}
