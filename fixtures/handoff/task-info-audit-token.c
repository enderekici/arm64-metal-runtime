#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int check_short_count(void) {
  task_info_data_t info;
  task_info_data_t before;
  memset(info, 0xa5, sizeof(info));
  memset(before, 0xa5, sizeof(before));
  mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT - 1;
  const kern_return_t result = task_info(mach_task_self(), TASK_AUDIT_TOKEN, info, &count);
  return result == KERN_INVALID_ARGUMENT && count == TASK_AUDIT_TOKEN_COUNT - 1 &&
      memcmp(info, before, sizeof(info)) == 0;
}

static int check_token_count(mach_msg_type_number_t requested) {
  task_info_data_t info;
  memset(info, 0, sizeof(info));
  mach_msg_type_number_t count = requested;
  const kern_return_t result = task_info(mach_task_self(), TASK_AUDIT_TOKEN, info, &count);
  if (result != KERN_SUCCESS || count != TASK_AUDIT_TOKEN_COUNT) return 0;
  audit_token_t token;
  memcpy(&token, info, sizeof(token));
  return audit_token_to_pid(token) == getpid() &&
      audit_token_to_pidversion(token) != 0 &&
      audit_token_to_auid(token) == getuid() &&
      audit_token_to_euid(token) == geteuid() &&
      audit_token_to_ruid(token) == getuid() &&
      audit_token_to_egid(token) == getegid() &&
      audit_token_to_rgid(token) == getgid();
}

int main(void) {
  const int short_count = check_short_count();
  const int exact_count = check_token_count(TASK_AUDIT_TOKEN_COUNT);
  const int oversized_count = check_token_count(TASK_AUDIT_TOKEN_COUNT + 1);
  const int pass = short_count && exact_count && oversized_count;
  printf("short=%d exact=%d oversized=%d result=%s\n", short_count, exact_count,
      oversized_count, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
