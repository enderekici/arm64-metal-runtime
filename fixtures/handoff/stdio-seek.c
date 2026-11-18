#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *file = fopen(argv[1], "rb");
    if (!file || ftell(file) != 0 || fgetc(file) != 'z' || ungetc('X', file) != 'X') return 2;
    if (fseek(file, 1, SEEK_CUR) != 0 || ftello(file) != 1 || fgetc(file) != 'e') return 3;
    if (fseeko(file, -2, SEEK_CUR) != 0 || fgetc(file) != 'z') return 4;
    if (fseeko(file, ((off_t)1 << 33) + 2, SEEK_SET) != 0) return 5;
    if (ftell(file) != ((off_t)1 << 33) + 2 || fgetc(file) != EOF || !feof(file)) return 6;
    if (fseeko(file, -((off_t)1 << 33), SEEK_CUR) != 0 || feof(file)) return 7;
    if (ftello(file) != 2 || fgetc(file) != 't') return 8;
    if (fseek(file, -1, SEEK_END) != 0 || fgetc(file) != '\n') return 9;
    errno = 0;
    if (fseek(file, 0, 999) != -1 || errno != EINVAL || fclose(file)) return 10;

    int ends[2];
    if (pipe(ends) || write(ends[1], "Q", 1) != 1 || close(ends[1])) return 11;
    file = fdopen(ends[0], "r");
    if (!file) return 12;
    errno = 0;
    if (fseeko(file, 0, SEEK_SET) != -1 || errno != ESPIPE) return 13;
    errno = 0;
    if (ftello(file) != -1 || errno != ESPIPE) return 14;
    if (fgetc(file) != 'Q' || fclose(file)) return 15;
    puts("stdio seek ok");
    return 0;
}
