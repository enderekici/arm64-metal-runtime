#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return 3;
    FILE *file = fdopen(fd, "rb");
    if (!file || fileno(file) != fd || setvbuf(file, NULL, _IOFBF, 4096)) return 4;
    if (fgetc(file) != 'z' || fclose(file)) return 5;
    errno = 0;
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 6;

    fd = open(argv[1], O_WRONLY);
    if (fd < 0) return 7;
    errno = 0;
    if (fdopen(fd, "r") != NULL || errno != EINVAL || fcntl(fd, F_GETFD) == -1) return 8;
    if (close(fd)) return 9;
    errno = 0;
    if (fdopen(-1, "r") != NULL || errno != EBADF) return 10;
    puts("fdopen lifecycle ok");
    return 0;
}
