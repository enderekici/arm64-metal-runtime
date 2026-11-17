#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int rejected_spawn(const char *path, int expected_error) {
  char *bad_argv[] = {"unsupported-image", NULL};
  char *empty_environment[] = {NULL};
  pid_t child = 12345;
  const int error = posix_spawn(&child, path, NULL, NULL, bad_argv, empty_environment);
  int result_ok = error == expected_error && child == 12345;
  if (error == 0 && child > 0) {
    int status = 0;
    (void)waitpid(child, &status, 0);
  }
  int status = 0;
  errno = 0;
  const pid_t waited = waitpid(12345, &status, WNOHANG);
  return result_ok && waited == -1 && errno == ECHILD;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "child") == 0) {
    const char *probe = getenv("METAL_SPAWN_PROBE");
    const char *runtimeOption = getenv("ARM64_METAL_SHARED_CACHE");
    const char *interpreterLibrary = getenv("ARM64_METAL_INTERPRETER_LIBRARY");
    const int valid = strcmp(argv[0], "spawn-probe-child-argv0") == 0 &&
                      strcmp(argv[1], "child") == 0 && probe != NULL &&
                      strcmp(probe, "yes") == 0 && runtimeOption != NULL &&
                      strcmp(runtimeOption, "1") == 0 && interpreterLibrary != NULL &&
                      strcmp(interpreterLibrary,
                             "/tmp/guest-env-must-not-configure-host-runtime") == 0;
    const char *out = valid ? "child-stdout argv/env=ok\n" : "child-stdout argv/env=bad\n";
    const char *err = valid ? "child-stderr argv/env=ok\n" : "child-stderr argv/env=bad\n";
    (void)write(STDOUT_FILENO, out, strlen(out));
    (void)write(STDERR_FILENO, err, strlen(err));
    return valid ? 23 : 24;
  }
  if (argc == 2 && strcmp(argv[1], "trap") == 0) {
    (void)raise(SIGTRAP);
    return 99;
  }

  char *child_argv[] = {"spawn-probe-child-argv0", "child", NULL};
  char *child_env[] = {"METAL_SPAWN_PROBE=yes", "ARM64_METAL_SHARED_CACHE=1",
                       "ARM64_METAL_INTERPRETER=1",
                       "ARM64_METAL_INTERPRETER_LIBRARY=/tmp/guest-env-must-not-configure-host-runtime",
                       NULL};
  pid_t child = -1;
  int spawn_error = posix_spawn(&child, argv[0], NULL, NULL, child_argv, child_env);
  if (spawn_error != 0) {
    fprintf(stderr, "spawn-error=%d\n", spawn_error);
    return 10;
  }
  int status = 0;
  pid_t waited = waitpid(child, &status, 0);
  int wait_error = errno;
  int second_status = 0;
  errno = 0;
  pid_t second_waited = waitpid(child, &second_status, WNOHANG);
  int second_error = errno;

  char text_directory[] = "/tmp/metal-spawn-invalid.XXXXXX";
  if (mkdtemp(text_directory) == NULL) return 11;
  char text_path[1024];
  if (snprintf(text_path, sizeof(text_path), "%s/not-mach-o", text_directory) >=
      (int)sizeof(text_path)) return 12;
  int text_file = open(text_path, O_WRONLY | O_CREAT | O_EXCL, 0700);
  if (text_file < 0 || write(text_file, "not a Mach-O\n", 13) != 13 || close(text_file) != 0)
    return 13;
  const int rejected_directory = rejected_spawn("/tmp", EACCES);
  const int rejected_text = rejected_spawn(text_path, ENOEXEC);
  (void)unlink(text_path);
  (void)rmdir(text_directory);

  char *trap_argv[] = {"spawn-probe-child-argv0", "trap", NULL};
  pid_t trap_child = -1;
  int trap_spawn_error = posix_spawn(&trap_child, argv[0], NULL, NULL, trap_argv,
                                     child_env);
  int trap_status = 0;
  pid_t trap_waited = trap_spawn_error == 0
      ? waitpid(trap_child, &trap_status, 0) : -1;
  const int trapped = trap_spawn_error == 0 && trap_waited == trap_child &&
                      WIFSIGNALED(trap_status) && WTERMSIG(trap_status) == SIGTRAP;

  printf("parent child=%d waited=%d wait_error=%d exited=%d exit=%d second=%d "
         "second_errno=%d bad_directory=%d bad_text=%d no_zombie=%d trapped=%d\n",
         child, (int)waited, wait_error, WIFEXITED(status), WEXITSTATUS(status),
         (int)second_waited, second_error, rejected_directory, rejected_text,
         rejected_directory && rejected_text, trapped);
  return !(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 23 &&
           second_waited == -1 && second_error == ECHILD && rejected_directory &&
           rejected_text && trapped);
}
