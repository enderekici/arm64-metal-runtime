#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile uint32_t global_word;

int main(void) {
  const pid_t parent = getpid();
  const uint32_t seed = (uint32_t)parent;
  volatile uint32_t stack_word = seed + 1;
  volatile uint32_t *heap_word = malloc(sizeof(*heap_word));
  int release_child[2], report[2];
  if (!heap_word || pipe(release_child) || pipe(report)) return 10;
  global_word = seed + 2;
  *heap_word = seed + 3;

  const pid_t child = fork();
  if (child < 0) return 11;
  if (child == 0) {
    close(release_child[1]);
    close(report[0]);
    char token = 0;
    if (read(release_child[0], &token, 1) != 1 || token != 'R') _exit(20);
    if (stack_word != seed + 1 || global_word != seed + 2 ||
      *heap_word != seed + 3) _exit(21);
    volatile uint32_t *fresh = malloc(sizeof(*fresh));
    if (!fresh) _exit(23);
    *fresh = seed + 9;
    if (*heap_word != seed + 3) _exit(24);
    stack_word = global_word = *heap_word = seed + 8;
    free((void *)fresh);
    free((void *)heap_word);
    const pid_t child_pid = getpid();
    if (child_pid == parent) _exit(22);
    if (write(report[1], &child_pid, sizeof(child_pid)) != sizeof(child_pid)) _exit(25);
    _exit(37);
  }

  close(release_child[0]);
  close(report[1]);
  int status = 0;
  if (waitpid(child, &status, WNOHANG) != 0) return 30;
  stack_word = global_word = *heap_word = seed + 7;
  if (write(release_child[1], "R", 1) != 1) return 31;
  close(release_child[1]);
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
    WEXITSTATUS(status) != 37) return 32;
  pid_t observed_pid = 0;
  if (read(report[0], &observed_pid, sizeof(observed_pid)) != sizeof(observed_pid) ||
    observed_pid != child) return 33;
  close(report[0]);
  if (stack_word != seed + 7 || global_word != seed + 7 ||
    *heap_word != seed + 7) return 34;
  errno = 0;
  if (waitpid(child, &status, 0) != -1 || errno != ECHILD) return 35;
  free((void *)heap_word);
  puts("fork isolation ok");
  return 0;
}
