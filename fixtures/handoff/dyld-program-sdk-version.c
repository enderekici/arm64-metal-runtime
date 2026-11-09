#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>

extern const struct mach_header_64 _mh_execute_header;
extern uint32_t dyld_get_program_sdk_version(void);
extern uint32_t dyld_get_program_min_os_version(void);

static int executable_versions(uint32_t *minimum, uint32_t *sdk) {
    const struct mach_header_64 *header = &_mh_execute_header;
    const uint8_t *cursor = (const uint8_t *)(header + 1);
    const uint8_t *end = cursor + header->sizeofcmds;
    *minimum = 0;
    *sdk = 0;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if (cursor > end || (size_t)(end - cursor) < sizeof(struct load_command)) return 0;
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmdsize < sizeof(*command) || (size_t)(end - cursor) < command->cmdsize)
            return 0;
        if (command->cmd == LC_BUILD_VERSION &&
            command->cmdsize >= sizeof(struct build_version_command)) {
            const struct build_version_command *build =
                (const struct build_version_command *)cursor;
            *minimum = build->minos;
            *sdk = build->sdk;
            return 1;
        }
        cursor += command->cmdsize;
    }
    return 0;
}

int main(void) {
    uint32_t expectedMinimum = 0;
    uint32_t expectedSdk = 0;
    if (!executable_versions(&expectedMinimum, &expectedSdk)) return 2;
    const uint32_t actualSdk = dyld_get_program_sdk_version();
    const uint32_t actualMinimum = dyld_get_program_min_os_version();
    printf("dyld_sdk=%u main_sdk=%u dyld_min_os=%u main_min_os=%u\n",
           actualSdk, expectedSdk, actualMinimum, expectedMinimum);
    return actualSdk == expectedSdk && actualMinimum == expectedMinimum ? 0 : 1;
}
