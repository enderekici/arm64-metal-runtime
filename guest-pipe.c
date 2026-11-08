#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    int ends[2];
    char bytes[5];
    if (pipe(ends) || fcntl(ends[0], F_SETFL, O_NONBLOCK) == -1) return 1;
    if (write(ends[1], "hello", 5) != 5 || close(ends[1])) return 2;
    if (read(ends[0], bytes, 5) != 5 || close(ends[0])) return 3;
    char name[32];
    if (snprintf(name, sizeof(name), "/am-shm-%d", getpid()) < 0) return 5;
    int shared = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shared < 0) return 6;
    int duplicate = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    int collision = duplicate < 0 && errno == EEXIST;
    if (duplicate >= 0) close(duplicate);
    int sized = ftruncate(shared, 4096) == 0;
    int reader = shm_open(name, O_RDONLY);
    volatile const char *view = reader < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, reader, 0);
    int removed = shm_unlink(name);
    int missing = shm_open(name, O_RDONLY);
    int absent = missing < 0 && errno == ENOENT;
    if (missing >= 0) close(missing);
    struct stat info;
    int live = reader >= 0 && fstat(reader, &info) == 0 && (info.st_mode & 0777) == 0600;
    if (reader >= 0) close(reader);
    if (close(shared) || !collision || removed || !absent || !live || !sized) return 7;
    if (view == MAP_FAILED || (uintptr_t)view % (uintptr_t)sysconf(_SC_PAGESIZE) ||
        view[0] != 0 || view[4095] != 0 || munmap((void *)view, 4096)) return 8;
    return write(STDOUT_FILENO, bytes, 5) == 5 ? 0 : 4;
}
