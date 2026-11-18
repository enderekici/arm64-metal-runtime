#include <string.h>

int main(void) {
    char buffer[6] = "ab";
    if (strlcat(buffer, "cd", sizeof(buffer)) != 4 || strcmp(buffer, "abcd")) return 1;
    if (strlcat(buffer, "xyz", sizeof(buffer)) != 7 || strcmp(buffer, "abcdx")) return 2;
    if (strlcat(buffer, "z", 2) != 3 || strcmp(buffer, "abcdx")) return 3;
    return 0;
}
