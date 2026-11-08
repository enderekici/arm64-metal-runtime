#include <stdio.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

extern const char *_dyld_get_objc_selector(const char *name);

static int (*volatile print_version)(const char *) = puts;

int main(void) {
    if (vm_page_size != (vm_size_t)sysconf(_SC_PAGESIZE)) return 7;
    if (((vm_size_t)1 << vm_page_shift) != vm_page_size || vm_kernel_page_size < vm_page_size) return 8;
    int32_t linked_version = NSVersionOfLinkTimeLibrary("z");
    if (linked_version <= 0 || linked_version != NSVersionOfLinkTimeLibrary("libz.1.dylib") ||
        NSVersionOfLinkTimeLibrary("am_runtime_missing_library") != -1) return 9;
    printf("linked z: %d\n", linked_version);
    const char *selector = _dyld_get_objc_selector("description");
    char repeated[] = "description";
    const char *other = _dyld_get_objc_selector("alloc");
    if (!selector || selector != _dyld_get_objc_selector(repeated) ||
        strcmp(selector, repeated) || !other || other == selector || strcmp(other, "alloc") ||
        _dyld_get_objc_selector("am_runtime_unique_missing_41fca829:")) return 6;
    const unsigned char input[] = "cache bytes";
    print_version(zlibVersion());
    puts(zError(Z_DATA_ERROR));
    printf("%lu\n", adler32(1, input, sizeof(input) - 1));
    const unsigned char stored[] = {
        0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff, 0x61, 0x62, 0x63, 0x02, 0x4d, 0x01, 0x27,
    };
    const unsigned char compressed[] = {
        0x78, 0x9c, 0xed, 0xca, 0xd1, 0x09, 0x80, 0x30, 0x0c, 0x05, 0xc0, 0x7f, 0xa7, 0x78,
        0x03, 0x88, 0x43, 0x74, 0x93, 0xd8, 0x44, 0x29, 0x04, 0x03, 0x8d, 0xa5, 0xeb, 0xd7,
        0x3d, 0x7c, 0x9f, 0x07, 0x57, 0x46, 0x73, 0x85, 0xc7, 0x9d, 0x3b, 0x54, 0x5e, 0x39,
        0x25, 0x0d, 0x3d, 0xe6, 0x47, 0x79, 0x14, 0x19, 0xa3, 0x57, 0xc3, 0xd5, 0xdc, 0xf2,
        0xd8, 0x0a, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0xf3, 0xbf, 0xf3,
        0x02, 0xcf, 0x35, 0xf3, 0x2e,
    };
    unsigned char output[16];
    unsigned long length = sizeof(output);
    if (uncompress(output, &length, stored, sizeof(stored)) != Z_OK || length != 3) return 1;
    unsigned char expanded[4096];
    unsigned long expanded_length = sizeof(expanded);
    if (uncompress(expanded, &expanded_length, compressed, sizeof(compressed)) != Z_OK ||
        expanded_length != 2880 || adler32(1, expanded, expanded_length) != 3476419374ul) return 4;
    if (fflush(stdout) != 0) return 2;
    if (write(STDOUT_FILENO, output, length) != (ssize_t)length) return 3;
    return write(STDOUT_FILENO, expanded, expanded_length) == (ssize_t)expanded_length ? 0 : 5;
}
