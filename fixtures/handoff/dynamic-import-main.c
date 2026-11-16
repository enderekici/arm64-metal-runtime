#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef size_t (*Length)(const char *);

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) return 3;
    void *symbol = dlsym(handle, "dynamic_import_length");
    Length length = NULL;
    memcpy(&length, &symbol, sizeof(symbol));
    if (length == NULL) return 4;
    const size_t result = length("synthetic import");
    printf("import_length=%zu\n", result);
    return result == 16 && dlclose(handle) == 0 ? 0 : 5;
}
