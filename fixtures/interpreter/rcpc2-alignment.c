#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

typedef uint64_t (*access_fn)(uint64_t, unsigned char *);
#define ACCESS(name, insn) \
    __attribute__((naked)) static uint64_t name( \
        uint64_t value, unsigned char *address) { __asm__("cmp x3, #8\n" insn "\n ret"); }
ACCESS(load8, "ldapurb w0, [x1]")
ACCESS(load16, "ldapurh w0, [x1]")
ACCESS(load32, "ldapur w0, [x1]")
ACCESS(load64, "ldapur x0, [x1]")
ACCESS(store8, "stlurb w0, [x1]")
ACCESS(store16, "stlurh w0, [x1]")
ACCESS(store32, "stlur w0, [x1]")
ACCESS(store64, "stlur x0, [x1]")
ACCESS(signed8, "ldapursb x0, [x1]")
ACCESS(signed16, "ldapursh x0, [x1]")
ACCESS(signed32, "ldapursw x0, [x1]")
ACCESS(signed8W, "ldapursb w0, [x1]")
ACCESS(signed16W, "ldapursh w0, [x1]")
ACCESS(ordinaryLoad, "ldur x0, [x1]")
ACCESS(ordinaryStore, "stur x0, [x1]")

// Keep each memory instruction in its own eligible typed block.
__attribute__((naked)) static uint64_t probe(uint64_t value, unsigned char *address,
                                             access_fn access, uint64_t operand,
                                             uint64_t *after) {
    __asm__("stp x29, x30, [sp, #-16]!\n"
            "blr x2\n"
            "mrs x1, nzcv\n"
            "str x1, [x4]\n"
            "ldp x29, x30, [sp], #16\n"
            "ret");
}

static uint32_t expected_flags(uint64_t operand) {
    return operand < 8 ? 0x80000000U : operand == 8 ? 0x60000000U : 0x20000000U;
}

static volatile sig_atomic_t caught;
static const uint64_t sentinel = 0x0123456789abcdefULL;
static void handler(int signum, siginfo_t *info, void *ctx) {
    ucontext_t *context = ctx;
    const _STRUCT_ARM_THREAD_STATE64 *state = &context->uc_mcontext->__ss;
    const _STRUCT_ARM_EXCEPTION_STATE64 *exception = &context->uc_mcontext->__es;
    const uint32_t instruction = *(const uint32_t *)state->__pc;
    const int store = ((instruction >> 22) & 3) == 0;
    const uint32_t syndrome = store ? 0x92000061U : 0x92000021U;
    if (signum != SIGBUS || info->si_code != BUS_ADRALN ||
        (uintptr_t)info->si_addr != state->__x[1] ||
        exception->__far != state->__x[1] || exception->__esr != syndrome ||
        (state->__cpsr & 0xf0000000U) != expected_flags(state->__x[3]) ||
        state->__pc != state->__x[2] + 4 || state->__x[0] != sentinel)
        _exit(90);
    ++caught;
    context->uc_mcontext->__ss.__pc += 4;
}

int main(int argc, char **argv) {
    _Alignas(16) unsigned char data[32];
    if (argc > 1) {
        // Default action on a crossing access, before installing a handler.
        (void)load64(0, data + 9);
        return 91;
    }
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    if (sigaction(SIGBUS, &action, NULL) != 0) return 1;

    static const struct {
        access_fn access;
        unsigned width, store, signedBits, ordered;
    } cases[] = {
        {load8, 1, 0, 0, 1}, {load16, 2, 0, 0, 1},
        {load32, 4, 0, 0, 1}, {load64, 8, 0, 0, 1},
        {store8, 1, 1, 0, 1}, {store16, 2, 1, 0, 1},
        {store32, 4, 1, 0, 1}, {store64, 8, 1, 0, 1},
        {signed8, 1, 0, 64, 1}, {signed16, 2, 0, 64, 1},
        {signed32, 4, 0, 64, 1}, {signed8W, 1, 0, 32, 1},
        {signed16W, 2, 0, 32, 1}, {ordinaryLoad, 8, 0, 0, 0},
        {ordinaryStore, 8, 1, 0, 0}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unsigned width = cases[i].width;
        for (unsigned offset = 0; offset < 16; ++offset) {
            const int crosses = cases[i].ordered && offset + width > 16;
            memset(data, 0xa5, sizeof(data));
            caught = 0;
            uint64_t operand = (offset * 7U) & 15U, after = 0;
            uint64_t result = probe(sentinel, data + offset, cases[i].access, operand, &after);
            if (caught != crosses || after != expected_flags(operand)) return 2;
            uint64_t expected = sentinel;
            if (!crosses && !cases[i].store) {
                expected = 0;
                if (cases[i].signedBits == 64) expected = UINT64_MAX;
                if (cases[i].signedBits == 32) expected = UINT32_MAX;
                memcpy(&expected, data + offset, width);
            }
            if (result != expected) return 3;
            for (unsigned byte = 0; byte < sizeof(data); ++byte) {
                const unsigned char expectedByte =
                    cases[i].store && !crosses && byte >= offset && byte < offset + width
                    ? (unsigned char)(sentinel >> ((byte - offset) * 8)) : 0xa5;
                if (data[byte] != expectedByte) return 4;
            }
        }
    }
    return 0;
}
