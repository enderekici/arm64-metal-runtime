#include <stdint.h>

struct event_qos {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    int32_t qos;
    uint64_t udata;
    uint32_t fflags;
    uint32_t xflags;
    int64_t data;
    uint64_t ext[4];
};

void *memcpy(void *destination, const void *source, unsigned long size) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (unsigned long index = 0; index < size; ++index) out[index] = in[index];
    return destination;
}

void *memset(void *destination, int value, unsigned long size) {
    unsigned char *out = destination;
    for (unsigned long index = 0; index < size; ++index) out[index] = (unsigned char)value;
    return destination;
}

static long sc8(long number, long a0, long a1, long a2, long a3,
                long a4, long a5, long a6, long a7) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x6 __asm__("x6") = a6;
    register long x7 __asm__("x7") = a7;
    int carry;
    __asm__ volatile("svc #0x80\n\tcset %w1, cs"
        : "+r"(x0), "=r"(carry)
        : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4),
          "r"(x5), "r"(x6), "r"(x7)
        : "memory", "cc");
    return carry ? -1 : x0;
}

__attribute__((noreturn)) static void finish(int status) {
    const char *message = status == 0 ? "kevent_qos two events ok\n"
                                      : "kevent_qos two events failed\n";
    sc8(4, 1, (long)message, status == 0 ? 25 : 29, 0, 0, 0, 0, 0);
    sc8(1, status, 0, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

__attribute__((noreturn)) void start(void) {
    struct event_qos changes[2] = {
        {.ident=0x111, .filter=-10, .flags=0x11, .qos=0,
         .udata=0xaaaa, .fflags=0x01000000},
        {.ident=0x222, .filter=-10, .flags=0x11, .qos=0,
         .udata=0xbbbb, .fflags=0x01000000},
    };
    struct event_qos ready[2] = {{0}};
    ready[0].ext[3] = 0xdeadbeefcafebabe;
    ready[1].ext[3] = 0xdeadbeefcafebabe;
    const long kq = sc8(362, 0, 0, 0, 0, 0, 0, 0, 0);
    if (kq < 0) finish(2);
    const long count = sc8(374, kq, (long)changes, 2, (long)ready, 2,
                           0, 0, 1);
    if (count != 2 || ready[0].ident != 0x111 || ready[1].ident != 0x222 ||
        ready[0].udata != 0xaaaa || ready[1].udata != 0xbbbb ||
        ready[0].ext[3] != 0 || ready[1].ext[3] != 0) finish(1);
    finish(0);
}
