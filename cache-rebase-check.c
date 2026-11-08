#include <dlfcn.h>
#include <inttypes.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2 || dlopen(argv[1], RTLD_LAZY) == NULL) return 1;
    intptr_t slide = 0;
    int found = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        if (strcmp(_dyld_get_image_name(i), argv[1]) == 0) {
            slide = _dyld_get_image_vmaddr_slide(i);
            found = 1;
            break;
        }
    }
    if (!found) return 2;
    unsigned checked = 0, authenticated = 0;
    uint64_t location, target;
    char line[256], details[128];
    const uint64_t address_mask = UINT64_C(0x0000ffffffffffff);
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%" SCNx64 " %" SCNx64 " %127[^\n]", &location, &target, details) != 3) return 3;
        uint64_t actual;
        memcpy(&actual, (const void *)(uintptr_t)(location + slide), sizeof(actual));
        uint64_t expected = target + slide;
        unsigned high;
        if (sscanf(details, "high-byte=%u", &high) == 1) {
            if (high > 255) return 5;
            expected |= (uint64_t)high << 56;
        } else {
            // Compare the address portion of PAC-signed native pointers.
            actual &= address_mask;
            expected &= address_mask;
            ++authenticated;
        }
        if (actual != expected) {
            fprintf(stderr, "target mismatch at 0x%" PRIx64 ": native 0x%" PRIx64 ", predicted 0x%" PRIx64 "\n",
                    location, actual, expected);
            return 4;
        }
        ++checked;
    }
    printf("%s: %u native pointer targets matched (%u authenticated locations)\n", argv[1], checked, authenticated);
    return checked && !ferror(stdin) ? 0 : 6;
}
