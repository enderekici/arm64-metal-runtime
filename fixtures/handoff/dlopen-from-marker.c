#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern void *dlopen_from(const char *path, int mode, void *addressInCaller);
extern int open_rpath_marker(void);

static __attribute__((noinline)) void caller_marker(void) {}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *loaderHandle = dlopen_from("@loader_path/libloader-marker-target.dylib",
                                     RTLD_NOW | RTLD_LOCAL, (void *)&caller_marker);
    int (*loaderValue)(void) = 0;
    if (loaderHandle != 0) {
        void *symbol = dlsym(loaderHandle, "dlopen_from_marker_value");
        __builtin_memcpy(&loaderValue, &symbol, sizeof(loaderValue));
    }
    void *callerHandle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    int (*rpathOpen)(void) = 0;
    if (callerHandle != 0) {
        void *symbol = dlsym(callerHandle, "open_rpath_marker");
        __builtin_memcpy(&rpathOpen, &symbol, sizeof(rpathOpen));
    }
    const int loaderResult = loaderValue == 0 ? -1 : loaderValue();
    const int rpathResult = rpathOpen == 0 ? -1 : rpathOpen();
    if (loaderHandle != 0) dlclose(loaderHandle);
    if (callerHandle != 0) dlclose(callerHandle);
    printf("loader_path=%d rpath=%d\n", loaderResult, rpathResult);
    return loaderResult == 41 && rpathResult == 73 ? 0 : 1;
}
