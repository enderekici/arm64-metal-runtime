#include <arpa/inet.h>
#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int csops_audittoken(pid_t, unsigned int, void *, size_t, audit_token_t *);

static int identity_call(void *buffer, size_t size, audit_token_t *token) {
  errno = 0;
  const int result = csops_audittoken(getpid(), 11U, buffer, size, token);
  return result == 0 ? 0 : errno;
}

int main(void) {
  task_info_data_t info = {0};
  mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
  const kern_return_t task_result = task_info(mach_task_self(), TASK_AUDIT_TOKEN, info,
      &count);
  audit_token_t token;
  memcpy(&token, info, sizeof(token));

  uint8_t full[0x408];
  memset(full, 0xa5, sizeof(full));
  const int full_error = task_result == KERN_SUCCESS
      ? identity_call(full, sizeof(full), &token) : -1;
  uint32_t full_length = 0;
  memcpy(&full_length, full + 4, sizeof(full_length));
  full_length = ntohl(full_length);
  const int full_ok = full_error == 0 && full_length >= 9 &&
      full_length <= sizeof(full) && memcmp(full, "\0\0\0\0", 4) == 0 &&
      full[full_length - 1] == 0 && strlen((const char *)full + 8) + 9 == full_length;

  uint8_t too_short[7];
  memset(too_short, 0xa5, sizeof(too_short));
  const int short_error = identity_call(too_short, sizeof(too_short), &token);
  const int short_untouched = memcmp(too_short, "\xa5\xa5\xa5\xa5\xa5\xa5\xa5", 7) == 0;

  uint8_t header_only[8];
  memset(header_only, 0xa5, sizeof(header_only));
  const int header_error = identity_call(header_only, sizeof(header_only), &token);
  uint32_t header_length = 0;
  memcpy(&header_length, header_only + 4, sizeof(header_length));
  header_length = ntohl(header_length);
  const int header_ok = header_error == ERANGE && header_length == full_length &&
      memcmp(header_only, "\0\0\0\0", 4) == 0;

  audit_token_t invalid = token;
  invalid.val[7] ^= 1U;
  uint8_t rejected[0x408];
  memset(rejected, 0xa5, sizeof(rejected));
  const int token_error = identity_call(rejected, sizeof(rejected), &invalid);
  const int token_untouched = rejected[0] == 0xa5 && rejected[7] == 0xa5;
  invalid = token;
  invalid.val[5] ^= 1U;
  const int pid_error = identity_call(rejected, sizeof(rejected), &invalid);
  const int pid_untouched = rejected[0] == 0xa5 && rejected[7] == 0xa5;
  const int null_token_error = identity_call(rejected, sizeof(rejected), NULL);
  const int null_token_untouched = rejected[0] == 0xa5 && rejected[7] == 0xa5;
  const int bad_token_pointer_error = identity_call(rejected, sizeof(rejected),
      (audit_token_t *)(uintptr_t)1);
  const int bad_token_pointer_untouched = rejected[0] == 0xa5 && rejected[7] == 0xa5;
  const int bad_output_error = identity_call((void *)(uintptr_t)1, sizeof(rejected), &token);
  const int null_output_short_error = identity_call(NULL, 7, &token);

  const int pass = task_result == KERN_SUCCESS && count == TASK_AUDIT_TOKEN_COUNT &&
      audit_token_to_pid(token) == getpid() && full_ok && short_error == ERANGE &&
      short_untouched && header_ok && token_error == ESRCH && token_untouched &&
      pid_error == ESRCH && pid_untouched &&
      null_token_error == EINVAL && null_token_untouched &&
      bad_token_pointer_error == EFAULT && bad_token_pointer_untouched &&
      bad_output_error == EFAULT && null_output_short_error == ERANGE;
  printf("identity=%s short=%d header=%d token=%d pid=%d null=%d badtoken=%d "
         "badoutput=%d nullshort=%d result=%s\n",
      full_ok ? (const char *)full + 8 : "invalid", short_error, header_error,
      token_error, pid_error, null_token_error, bad_token_pointer_error,
      bad_output_error, null_output_short_error, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
