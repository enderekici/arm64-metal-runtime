#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern void *__memset_chk(void *destination, int value, size_t count,
                          size_t destination_size);

struct trap_record {
  int32_t signal_number;
  int32_t signal_code;
  uint64_t program_counter;
  uint64_t fault_address;
};

static char destination[8];

static void trap_handler(int signal_number, siginfo_t *information, void *context) {
  const ucontext_t *machine_context = (const ucontext_t *)context;
  const struct trap_record record = {
      signal_number,
      information->si_code,
      machine_context->uc_mcontext->__ss.__pc,
      (uint64_t)information->si_addr,
  };
  (void)write(STDOUT_FILENO, &record, sizeof(record));
  _exit(77);
}

int main(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = trap_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTRAP, &action, 0) != 0) return 91;
  (void)__memset_chk(destination, 'x', 9, sizeof(destination));
  return 90;
}
