#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static atomic_int waitStarted;
static atomic_int siblingReleased;
static char releasePath[1024];
static pid_t childPid;

struct waiter_result {
  pid_t waited;
  int status;
  int error;
};

static void *wait_for_child(void *opaque) {
  struct waiter_result *result = (struct waiter_result *)opaque;
  result->waited = waitpid(childPid, &result->status, 0);
  result->error = errno;
  return NULL;
}

static void *release_child(void *unused) {
  (void)unused;
  while (!atomic_load_explicit(&waitStarted, memory_order_acquire)) (void)sched_yield();
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
  (void)nanosleep(&pause, NULL);
  int descriptor = open(releasePath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor >= 0) {
    (void)write(descriptor, "go", 2);
    (void)close(descriptor);
    atomic_store_explicit(&siblingReleased, 1, memory_order_release);
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "wait-child") == 0) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 25;
    const long long deadline = (long long)now.tv_sec * 1000000000LL + now.tv_nsec +
                               5000000000LL;
    while ((long long)now.tv_sec * 1000000000LL + now.tv_nsec < deadline) {
      if (access(argv[2], F_OK) == 0) return 23;
      (void)sched_yield();
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 25;
    }
    return 24;
  }

  char directory[] = "/tmp/metal-cooperative-wait.XXXXXX";
  if (mkdtemp(directory) == NULL) return 10;
  if (snprintf(releasePath, sizeof(releasePath), "%s/release", directory) >=
      (int)sizeof(releasePath)) return 11;

  pthread_t sibling;
  if (pthread_create(&sibling, NULL, release_child, NULL) != 0) return 12;
  char *child_argv[] = {argv[0], "wait-child", releasePath, NULL};
  childPid = -1;
  int spawn_error = posix_spawn(&childPid, argv[0], NULL, NULL, child_argv, environ);
  if (spawn_error != 0) return 13;
  int nohang_status = 0;
  errno = 0;
  const pid_t nohang = waitpid(childPid, &nohang_status, WNOHANG);
  struct waiter_result first = {-1, 0, 0};
  struct waiter_result second = {-1, 0, 0};
  pthread_t firstWaiter;
  pthread_t secondWaiter;
  if (pthread_create(&firstWaiter, NULL, wait_for_child, &first) != 0 ||
      pthread_create(&secondWaiter, NULL, wait_for_child, &second) != 0) return 14;
  atomic_store_explicit(&waitStarted, 1, memory_order_release);
  (void)pthread_join(firstWaiter, NULL);
  (void)pthread_join(secondWaiter, NULL);
  (void)pthread_join(sibling, NULL);
  const int firstMatches = first.waited == childPid;
  const int secondMatches = second.waited == childPid;
  const struct waiter_result *reaped = firstMatches ? &first : &second;
  const struct waiter_result *lost = firstMatches ? &second : &first;
  const int singleReap = firstMatches != secondMatches && lost->waited == -1 &&
                         lost->error == ECHILD;
  const int exitedExpected = (firstMatches || secondMatches) &&
                             WIFEXITED(reaped->status) &&
                             WEXITSTATUS(reaped->status) == 23;
  printf("cooperative-wait released=%d nohang_zero=%d "
         "waited_matches=%d single_reap=%d exited23=%d\n",
         atomic_load_explicit(&siblingReleased, memory_order_acquire), nohang == 0,
         firstMatches || secondMatches,
         singleReap, exitedExpected);
  (void)unlink(releasePath);
  (void)rmdir(directory);
  return !(atomic_load_explicit(&siblingReleased, memory_order_acquire) &&
           nohang == 0 && singleReap &&
           exitedExpected);
}
