#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*stream_case)(void *);

#define STREAM_CASE(name, instructions)                                      \
  static void name(void *memory) {                                           \
    __asm__ volatile("smstart sm\n\t" instructions "\n\tsmstop sm\n\t"   \
                     :                                                        \
                     : "r"(memory)                                           \
                     : "memory", "v0", "v1", "v2", "x9");                 \
  }

STREAM_CASE(legacy_ldr_d, "ldr d0, [%0]")
STREAM_CASE(legacy_str_d, "str d0, [%0]")
STREAM_CASE(legacy_fmov_x_d, "fmov x9, d0")
STREAM_CASE(legacy_stp_q, "stp q0, q1, [%0]")
STREAM_CASE(legacy_ldp_q, "ldp q0, q1, [%0]")
STREAM_CASE(legacy_add_v, "add v0.4s, v1.4s, v2.4s")
STREAM_CASE(legacy_fadd_d, "fadd d0, d1, d2")
STREAM_CASE(legacy_fadd_s, "fadd s0, s1, s2")

static void streaming_sve_positive(void *memory) {
  (void)memory;
  __asm__ volatile("smstart sm\n\t"
                   "ptrue p0.s\n\t"
                   "mov z0.s, #3\n\t"
                   "cntw x9\n\t"
                   "smstop sm\n\t"
                   :
                   :
                   : "memory", "v0", "x9");
}

struct test_case {
  const char *name;
  stream_case run;
};

int main(void) {
  static uint8_t memory[64] __attribute__((aligned(16))) = {0};
  const struct test_case cases[] = {
      {"sve-streaming-control-positive", streaming_sve_positive},
      {"legacy-ldr-d", legacy_ldr_d},
      {"legacy-str-d", legacy_str_d},
      {"legacy-fmov-x-d", legacy_fmov_x_d},
      {"legacy-stp-q", legacy_stp_q},
      {"legacy-ldp-q", legacy_ldp_q},
      {"legacy-add-v", legacy_add_v},
      {"legacy-fadd-d", legacy_fadd_d},
      {"legacy-fadd-s", legacy_fadd_s},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
      cases[i].run(memory);
      _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 2;
    if (WIFEXITED(status)) {
      printf("%s exited %d\n", cases[i].name, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      printf("%s signal %d\n", cases[i].name, WTERMSIG(status));
    } else {
      printf("%s unexpected wait status 0x%x\n", cases[i].name, status);
      return 2;
    }
  }
  return 0;
}
