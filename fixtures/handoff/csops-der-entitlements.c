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

static int der_entitlements_call(void *buffer, size_t size, audit_token_t *token) {
  errno = 0;
  const int result = csops_audittoken(getpid(), 16U, buffer, size, token);
  return result == 0 ? 0 : errno;
}

static uint64_t fingerprint(const uint8_t *bytes, size_t size) {
  uint64_t value = UINT64_C(14695981039346656037);
  for (size_t index = 0; index < size; ++index) {
    value ^= bytes[index];
    value *= UINT64_C(1099511628211);
  }
  return value;
}

static int is_sentinel(const uint8_t *bytes, size_t size) {
  for (size_t index = 0; index < size; ++index) {
    if (bytes[index] != 0xa5) return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2 || (strcmp(argv[1], "der") != 0 && strcmp(argv[1], "absent") != 0)) {
    return 2;
  }
  const int expect_der = strcmp(argv[1], "der") == 0;
  task_info_data_t info = {0};
  mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
  const kern_return_t task_result = task_info(mach_task_self(), TASK_AUDIT_TOKEN,
      info, &count);
  audit_token_t token;
  memcpy(&token, info, sizeof(token));

  uint8_t full[0x408];
  memset(full, 0xa5, sizeof(full));
  const int full_error = task_result == KERN_SUCCESS
      ? der_entitlements_call(full, sizeof(full), &token) : -1;
  uint32_t magic = 0;
  uint32_t length = 0;
  memcpy(&magic, full, sizeof(magic));
  memcpy(&length, full + 4, sizeof(length));
  magic = ntohl(magic);
  length = ntohl(length);
  const int full_untouched = is_sentinel(full, sizeof(full));
  const int der_ok = full_error == 0 && magic == 0xfade7172 &&
      length >= 8 && length < sizeof(full) && full[length] == 0xa5;
  const int absent_ok = full_error == 0 && full_untouched;
  if (magic != 0xfade7172) length = 0;

  uint8_t short_buffer[7];
  memset(short_buffer, 0xa5, sizeof(short_buffer));
  const int short_error = der_entitlements_call(short_buffer, sizeof(short_buffer), &token);
  const int short_untouched = is_sentinel(short_buffer, sizeof(short_buffer));

  uint8_t header[8];
  memset(header, 0xa5, sizeof(header));
  const int header_error = der_entitlements_call(header, sizeof(header), &token);
  uint32_t header_length = 0;
  memcpy(&header_length, header + 4, sizeof(header_length));
  header_length = ntohl(header_length);
  const int header_untouched = is_sentinel(header, sizeof(header));
  const int header_ok = expect_der
      ? header_error == ERANGE && header_length == length &&
          memcmp(header, "\0\0\0\0", 4) == 0
      : header_error == 0 && header_untouched;

  audit_token_t invalid = token;
  invalid.val[7] ^= 1U;
  uint8_t rejected[0x408];
  memset(rejected, 0xa5, sizeof(rejected));
  const int token_error = der_entitlements_call(rejected, sizeof(rejected), &invalid);
  const int token_untouched = is_sentinel(rejected, sizeof(rejected));
  const int null_error = der_entitlements_call(rejected, sizeof(rejected), NULL);
  const int null_untouched = is_sentinel(rejected, sizeof(rejected));
  const int bad_token_error = der_entitlements_call(rejected, sizeof(rejected),
      (audit_token_t *)(uintptr_t)1);
  const int bad_token_untouched = is_sentinel(rejected, sizeof(rejected));
  const int bad_output_error = der_entitlements_call((void *)(uintptr_t)1,
      sizeof(rejected), &token);
  const int short_bad_output_error = der_entitlements_call((void *)(uintptr_t)1, 7, &token);

  const int pass = task_result == KERN_SUCCESS && count == TASK_AUDIT_TOKEN_COUNT &&
      audit_token_to_pid(token) == getpid() && (expect_der ? der_ok : absent_ok) &&
      short_error == (expect_der ? ERANGE : 0) &&
      short_untouched && header_ok &&
      token_error == ESRCH && token_untouched && null_error == EINVAL &&
      null_untouched && bad_token_error == EFAULT && bad_token_untouched &&
      bad_output_error == (expect_der ? EFAULT : 0) &&
      short_bad_output_error == (expect_der ? ERANGE : 0);
  printf("mode=%s full=%d blob=%d length=%u fnv=%016llx short7=%d header=%d "
         "token=%d null=%d badtoken=%d badoutput=%d shortbadoutput=%d result=%s\n",
      argv[1], full_error, der_ok, length,
      (unsigned long long)(der_ok ? fingerprint(full, length) : 0), short_error,
      header_error, token_error, null_error, bad_token_error, bad_output_error,
      short_bad_output_error, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
