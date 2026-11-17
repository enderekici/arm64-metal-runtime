#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char input_text[] = "hello pipeline\n";
static const char output_text[] = "HELLO PIPELINE\n";

static int write_all(int fd, const char *buffer, size_t length) {
  size_t written = 0;
  while (written < length) {
    const ssize_t result = write(fd, buffer + written, length - written);
    if (result < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (result == 0) return -1;
    written += (size_t)result;
  }
  return 0;
}

static int child_main(void) {
  char buffer[sizeof(input_text)];
  size_t length = 0;
  for (;;) {
    const ssize_t result = read(STDIN_FILENO, buffer + length,
                                sizeof(buffer) - length);
    if (result < 0) {
      if (errno == EINTR) continue;
      return 41;
    }
    if (result == 0) break;
    length += (size_t)result;
    if (length == sizeof(buffer)) return 41;
  }

  if (length != sizeof(input_text) - 1 ||
      memcmp(buffer, input_text, sizeof(input_text) - 1) != 0) {
    return 41;
  }
  if (write_all(STDOUT_FILENO, output_text, sizeof(output_text) - 1) != 0) {
    return 42;
  }
  return 23;
}

static int expect_stale_source_failure(const char *path) {
  int descriptors[2];
  if (pipe(descriptors) != 0) return -1;
  const int stale_source = descriptors[0];
  if (close(stale_source) != 0) {
    close(descriptors[1]);
    return -1;
  }

  posix_spawn_file_actions_t actions;
  int result = posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    close(descriptors[1]);
    return -1;
  }
  result = posix_spawn_file_actions_adddup2(&actions, stale_source, STDIN_FILENO);
  if (result != 0) goto done;
  result = posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
  if (result != 0) goto done;

  char *child_argv[] = {(char *)path, (char *)"--child", NULL};
  pid_t child = (pid_t)-1;
  result = posix_spawn(&child, path, &actions, NULL, child_argv, environ);
  if (result == 0) {
    int status = 0;
    (void)waitpid(child, &status, 0);
    result = -1;
  } else if (result == EBADF && child == (pid_t)-1) {
    result = 0;
  } else {
    result = -1;
  }

done:
  (void)posix_spawn_file_actions_destroy(&actions);
  close(descriptors[1]);
  return result == 0 ? 0 : -1;
}

static int parent_main(const char *path) {
  int child_input[2];
  int child_output[2];
  if (pipe(child_input) != 0) return 1;
  if (pipe(child_output) != 0) {
    close(child_input[0]);
    close(child_input[1]);
    return 1;
  }

  posix_spawn_file_actions_t actions;
  int result = posix_spawn_file_actions_init(&actions);
  if (result != 0) goto pipe_failure;
  result = posix_spawn_file_actions_adddup2(&actions, child_input[0], STDIN_FILENO);
  if (result != 0) goto actions_failure;
  result = posix_spawn_file_actions_adddup2(&actions, child_output[1], STDOUT_FILENO);
  if (result != 0) goto actions_failure;
  result = posix_spawn_file_actions_addclose(&actions, child_input[0]);
  if (result != 0) goto actions_failure;
  result = posix_spawn_file_actions_addclose(&actions, child_input[1]);
  if (result != 0) goto actions_failure;
  result = posix_spawn_file_actions_addclose(&actions, child_output[0]);
  if (result != 0) goto actions_failure;
  result = posix_spawn_file_actions_addclose(&actions, child_output[1]);
  if (result != 0) goto actions_failure;

  char *child_argv[] = {(char *)path, (char *)"--child", NULL};
  pid_t child = (pid_t)-1;
  result = posix_spawn(&child, path, &actions, NULL, child_argv, environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  if (result != 0) goto pipe_failure;

  close(child_input[0]);
  close(child_output[1]);
  if (write_all(child_input[1], input_text, sizeof(input_text) - 1) != 0) {
    close(child_input[1]);
    close(child_output[0]);
    (void)waitpid(child, NULL, 0);
    return 1;
  }
  close(child_input[1]);

  char output[sizeof(output_text)];
  size_t length = 0;
  for (;;) {
    const ssize_t count = read(child_output[0], output + length,
                               sizeof(output) - length);
    if (count < 0) {
      if (errno == EINTR) continue;
      close(child_output[0]);
      (void)waitpid(child, NULL, 0);
      return 1;
    }
    if (count == 0) break;
    length += (size_t)count;
    if (length == sizeof(output)) {
      close(child_output[0]);
      (void)waitpid(child, NULL, 0);
      return 1;
    }
  }
  close(child_output[0]);

  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 23 || length != sizeof(output_text) - 1 ||
      memcmp(output, output_text, sizeof(output_text) - 1) != 0 ||
      expect_stale_source_failure(path) != 0) {
    return 1;
  }

  puts("posix_spawn pipeline ok");
  return 0;

actions_failure:
  (void)posix_spawn_file_actions_destroy(&actions);
pipe_failure:
  close(child_input[0]);
  close(child_input[1]);
  close(child_output[0]);
  close(child_output[1]);
  return 1;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--child") == 0) return child_main();
  if (argc != 1) return 2;
  return parent_main(argv[0]);
}
