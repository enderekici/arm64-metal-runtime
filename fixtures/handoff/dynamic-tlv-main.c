#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef int (*GetValue)(void);
typedef void (*SetValue)(int);

static GetValue get_value;
static SetValue set_value;
static int worker_before;
static int worker_after;

static void *read_worker_tls(void *unused) {
    (void)unused;
    worker_before = get_value();
    set_value(31);
    worker_after = get_value();
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) return 3;

    void *symbol = dlsym(handle, "dynamic_tls_get");
    memcpy(&get_value, &symbol, sizeof(symbol));
    symbol = dlsym(handle, "dynamic_tls_set");
    memcpy(&set_value, &symbol, sizeof(symbol));
    if (get_value == NULL || set_value == NULL) return 4;

    const int main_before = get_value();
    set_value(21);
    pthread_t thread;
    if (pthread_create(&thread, NULL, read_worker_tls, NULL) != 0) return 5;
    if (pthread_join(thread, NULL) != 0) return 6;
    const int main_after = get_value();
    if (dlclose(handle) != 0) return 7;
    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) return 8;
    symbol = dlsym(handle, "dynamic_tls_get");
    memcpy(&get_value, &symbol, sizeof(symbol));
    symbol = dlsym(handle, "dynamic_tls_set");
    memcpy(&set_value, &symbol, sizeof(symbol));
    if (get_value == NULL || set_value == NULL) return 9;
    const int reopened = get_value();
    set_value(41);
    const int reopened_after = get_value();
    printf("main_before=%d worker_before=%d worker_after=%d main_after=%d "
           "reopened=%d reopened_after=%d\n",
           main_before, worker_before, worker_after, main_after, reopened, reopened_after);
    const int passed = main_before == 9 && worker_before == 9 &&
        worker_after == 31 && main_after == 21 && reopened == 21 && reopened_after == 41;
    if (dlclose(handle) != 0) return 10;
    return passed ? 0 : 11;
}
