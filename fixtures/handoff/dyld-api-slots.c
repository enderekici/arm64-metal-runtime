// Native-vs-guest probe for the dyld APIs vtable slots reached by libsystem_trace
// under java -version: shared-cache UUID, image header/path containing an address,
// and remove-image callback registration.
//   (no args)            probes; optional argv[1] = hex cache offset to probe
//   --find-appkit        native helper: print AppKit's cache offset (+0x1000)
//   --unmodeled          call _dyld_get_image_uuid, a slot the runtime does not model
//   --remove-dlclose LIB register a remove-image callback, dlopen+dlclose LIB
//   --remove-exit LIB    register, dlopen LIB, exit without dlclose
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <uuid/uuid.h>

extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);
extern const void* _dyld_get_shared_cache_range(size_t* length);
extern const struct mach_header* dyld_image_header_containing_address(const void* addr);
extern const char* dyld_image_path_containing_address(const void* addr);
extern void _dyld_register_func_for_remove_image(void (*func)(const struct mach_header*, intptr_t));
extern bool _dyld_get_image_uuid(const struct mach_header* mh, uuid_t uuid);
struct text_info { uint64_t version, loadAddressUnslid, textSegmentSize; uuid_t dylibUuid; const char* path; uint64_t textSegmentOffset; };
extern int dyld_shared_cache_iterate_text(const uuid_t, void (^)(const struct text_info*));

static int removed;
static void on_remove(const struct mach_header* mh, intptr_t slide) { (void)mh; (void)slide; ++removed; }

static const char* base(const char* path) {
    if (!path) return "(null)";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void probe(const char* label, const void* address) {
    const struct mach_header* header = dyld_image_header_containing_address(address);
    const char* path = dyld_image_path_containing_address(address);
    Dl_info info = {0};
    const int found = address && dladdr(address, &info);
    printf("%s header=%s dladdr_base=%s path=%s\n", label,
           header == NULL ? "null" : (found && header == info.dli_fbase ? "match" : "mismatch"),
           found ? "found" : "none", base(path));
    if (header && path && found && strcmp(path, info.dli_fname) != 0)
        printf("%s path differs from dladdr: %s vs %s\n", label, path, info.dli_fname);
}

static int find_appkit(void) {
    uuid_t uuid;
    _dyld_get_shared_cache_uuid(uuid);
    size_t length = 0;
    const uint8_t* cache = _dyld_get_shared_cache_range(&length);
    Dl_info libc = {0};
    dladdr((const void*)&printf, &libc);
    __block uint64_t libcUnslid = 0, appkitUnslid = 0;
    dyld_shared_cache_iterate_text(uuid, ^(const struct text_info* info) {
        if (strcmp(info->path, libc.dli_fname) == 0) libcUnslid = info->loadAddressUnslid;
        if (strstr(info->path, "/AppKit.framework/")) appkitUnslid = info->loadAddressUnslid;
    });
    const uint8_t* appkit = (const uint8_t*)libc.dli_fbase + (appkitUnslid - libcUnslid) + 0x1000;
    printf("%lx\n", (unsigned long)(appkit - cache));
    return 0;
}

static int remove_mode(const char* library, bool close) {
    _dyld_register_func_for_remove_image(on_remove);
    void* handle = dlopen(library, RTLD_NOW);
    printf("dlopen=%s removed_after_open=%d\n", handle ? "ok" : dlerror(), removed);
    if (close && handle) printf("dlclose=%d removed_after_close=%d\n", dlclose(handle), removed);
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--find-appkit") == 0) return find_appkit();
    if (argc > 2 && strcmp(argv[1], "--remove-dlclose") == 0) return remove_mode(argv[2], true);
    if (argc > 2 && strcmp(argv[1], "--remove-exit") == 0) return remove_mode(argv[2], false);
    if (argc > 1 && strcmp(argv[1], "--unmodeled") == 0) {
        uuid_t image;
        printf("unmodeled-call\n");
        fflush(stdout);
        printf("image-uuid ret=%d\n", _dyld_get_image_uuid(_dyld_get_image_header(0), image));
        fflush(stdout);
        return 0;
    }
    uuid_t uuid;
    memset(uuid, 0xa5, sizeof uuid);
    const bool haveUuid = _dyld_get_shared_cache_uuid(uuid);
    printf("cache-uuid ret=%d nonzero=%d\n", haveUuid, !uuid_is_null(uuid));
    uuid_string_t text;
    uuid_unparse_upper(uuid, text);
    printf("cache-uuid %s\n", text);
    probe("main", (const void*)&main);
    probe("main-header", (const void*)_dyld_get_image_header(0));
    probe("printf", (const void*)&printf);
    probe("dyld-api", (const void*)&dyld_image_path_containing_address);
    void* heap = malloc(64);
    probe("heap", heap);
    free(heap);
    probe("null", NULL);
    int stack = 0;
    probe("stack", &stack);
    size_t length = 0;
    const uint8_t* cache = _dyld_get_shared_cache_range(&length);
    // the last page of the cache mapping belongs to no image
    probe("cache-tail", cache ? cache + length - 16 : NULL);
    if (argc > 1) probe("cache-image", cache + strtoul(argv[1], NULL, 16));
    _dyld_register_func_for_remove_image(on_remove);
    printf("remove-registered immediate_calls=%d\n", removed);
    fflush(stdout);
    return 0;
}
