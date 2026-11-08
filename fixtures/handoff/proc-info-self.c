#include <errno.h>
#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/syscall.h>
#include <unistd.h>

#if SYS_proc_info != 336 || PROC_PIDT_SHORTBSDINFO != 13
#error "Expected the installed Darwin proc_info ABI"
#endif

#define PROC_PIDUNIQIDENTIFIERINFO 17

struct proc_uniqidentifierinfo_private {
  uint8_t p_uuid[16];
  uint64_t p_uniqueid;
  uint64_t p_puniqueid;
  uint64_t p_reserve2;
  uint64_t p_reserve3;
  uint64_t p_reserve4;
};

struct raw_result {
  uint64_t value;
  uint64_t failed;
};

static struct raw_result raw_proc_info(int call, int pid, unsigned flavor,
                                       uint64_t arg, void *buffer, int size) {
  register uint64_t x0 __asm__("x0") = (uint32_t)call;
  register uint64_t x1 __asm__("x1") = (uint32_t)pid;
  register uint64_t x2 __asm__("x2") = flavor;
  register uint64_t x3 __asm__("x3") = arg;
  register uint64_t x4 __asm__("x4") = (uint64_t)buffer;
  register uint64_t x5 __asm__("x5") = (uint32_t)size;
  register uint64_t x16 __asm__("x16") = SYS_proc_info;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %6, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
                     "+r"(x5), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static const uint8_t *main_uuid(void) {
  const struct mach_header_64 *header = &_mh_execute_header;
  const struct load_command *command = (const void *)(header + 1);
  for (uint32_t index = 0; index < header->ncmds; ++index) {
    if (command->cmd == LC_UUID) {
      return ((const struct uuid_command *)command)->uuid;
    }
    command = (const void *)((const uint8_t *)command + command->cmdsize);
  }
  return NULL;
}

static const char *leaf_name(const char *path) {
  const char *leaf = strrchr(path, '/');
  return leaf == NULL ? path : leaf + 1;
}

int main(int argc, char **argv) {
  if (argc != 1) return 2;
  const int pid = getpid();
  struct proc_bsdshortinfo short_info = {0};
  struct raw_result result = raw_proc_info(2, pid, PROC_PIDT_SHORTBSDINFO,
      1, &short_info, sizeof(short_info));
  if (result.failed || result.value != sizeof(short_info) ||
      short_info.pbsi_pid != (uint32_t)pid ||
      strncmp(short_info.pbsi_comm, leaf_name(argv[0]), sizeof(short_info.pbsi_comm) - 1) != 0) {
    return 3;
  }

  uint64_t unique_words[8] = {0};
  unique_words[7] = 99;
  result = raw_proc_info(2, pid, PROC_PIDUNIQIDENTIFIERINFO, 0,
      unique_words, sizeof(unique_words));
  const uint8_t *uuid = main_uuid();
  if (result.failed || result.value != sizeof(struct proc_uniqidentifierinfo_private) ||
      unique_words[2] == 0 || unique_words[7] != 99 || uuid == NULL ||
      memcmp(unique_words, uuid, 16) != 0) {
    return 4;
  }

  result = raw_proc_info(2, pid, PROC_PIDUNIQIDENTIFIERINFO, 0,
      unique_words, sizeof(struct proc_uniqidentifierinfo_private) - 1);
  if (!result.failed || result.value != ENOMEM) return 5;

  result = raw_proc_info(2, pid, PROC_PIDUNIQIDENTIFIERINFO, 0, NULL,
      sizeof(struct proc_uniqidentifierinfo_private));
  if (!result.failed || result.value != EFAULT) return 6;

  static const char output[] = "proc-info=ok\n";
  return write(STDOUT_FILENO, output, sizeof(output) - 1) ==
      (ssize_t)(sizeof(output) - 1) ? 0 : 7;
}
