#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expected_slide(const struct mach_header *header, intptr_t *slide) {
    if (header == NULL || header->magic != MH_MAGIC_64) return 0;
    const struct mach_header_64 *header64 = (const struct mach_header_64 *)header;
    const uint8_t *cursor = (const uint8_t *)(header64 + 1);
    for (uint32_t index = 0; index < header64->ncmds; ++index) {
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmdsize < sizeof(struct load_command)) return 0;
        if (command->cmd == LC_SEGMENT_64 &&
            command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment =
                (const struct segment_command_64 *)cursor;
            if (strncmp(segment->segname, "__TEXT", sizeof(segment->segname)) == 0) {
                *slide = (intptr_t)(uintptr_t)header - (intptr_t)segment->vmaddr;
                return 1;
            }
        }
        cursor += command->cmdsize;
    }
    return 0;
}

int main(void) {
    const uint32_t count = _dyld_image_count();
    if (_dyld_get_image_vmaddr_slide(count) != 0) {
        fprintf(stderr, "out-of-range image slide is not zero\n");
        return 1;
    }
    for (uint32_t index = 0; index < count; ++index) {
        const struct mach_header *header = _dyld_get_image_header(index);
        const char *name = _dyld_get_image_name(index);
        intptr_t expected = 0;
        if (name == NULL || !expected_slide(header, &expected)) {
            fprintf(stderr, "image %u has no valid name or __TEXT segment\n", index);
            return 2;
        }
        const intptr_t actual = _dyld_get_image_vmaddr_slide(index);
        if (actual != expected) {
            fprintf(stderr, "image %u slide mismatch: actual=%lld expected=%lld name=%s\n",
                    index, (long long)actual, (long long)expected, name);
            return 3;
        }
    }
    printf("images=%u oob-slide=0 all-indexed-slides-match-__TEXT\n", count);
    return 0;
}
