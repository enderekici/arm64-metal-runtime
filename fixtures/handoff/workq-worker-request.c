#include <errno.h>
#include <ptrauth.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#ifndef WORKQ_GUEST_FIXTURE
#include <stdio.h>
#include <sys/wait.h>
#endif

struct pthread_workqueue_config {
    uint32_t flags, version;
    void (*kevent_cb)(void **, int *);
    void (*workloop_cb)(uint64_t *, void **, int *);
    void (*workq_cb)(unsigned long);
    uint64_t queue_serialno_offs, queue_label_offs;
};
#ifndef WORKQ_GUEST_NO_INIT
extern int pthread_workqueue_setup(struct pthread_workqueue_config *, size_t);
#endif
#define PTHREAD_WORKQUEUE_CONFIG_VERSION 2

struct registration_data {
    uint64_t version, dispatch_queue_offset, main_qos;
    uint32_t tsd_offset, return_to_kernel_offset, mach_thread_self_offset;
    uint64_t stack_addr_hint;
    uint32_t mutex_default_policy, joinable_offset_bits, wq_quantum_expiry_offset;
} __attribute__((packed));
_Static_assert(sizeof(struct registration_data) == 56, "Darwin registration ABI size");
struct dispatch_config { uint32_t version, flags; uint64_t serial_offset, label_offset; };
struct trap_result { uint64_t value, carry; };
#ifndef WORKQ_GUEST_NO_INIT
static _Atomic unsigned callback_bits;
static _Atomic unsigned long callback_argument;
#endif
static _Atomic unsigned raw_entry_bits;
static struct dispatch_config raw_dispatch_config = { .version = 2 };

static struct trap_result darwin_syscall(uint64_t number, uint64_t a0, uint64_t a1,
        uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    register uint64_t x4 __asm__("x4") = a4;
    register uint64_t x5 __asm__("x5") = a5;
    register uint64_t x16 __asm__("x16") = number;
    register uint64_t x17 __asm__("x17");
    __asm__ volatile("svc #0x80\n\tcset x17, cs"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
          "+r"(x16), "=r"(x17) : : "memory", "cc");
    return (struct trap_result){x0, x17};
}

static struct trap_result thread_self_with_carry_seeded(void) {
    register uint64_t x0 __asm__("x0") = 0;
    register uint64_t x16 __asm__("x16") = 372;
    register uint64_t x17 __asm__("x17");
    __asm__ volatile("cmp xzr, xzr\n\tsvc #0x80\n\tcset x17, cs"
        : "+r"(x0), "+r"(x16), "=r"(x17) : : "memory", "cc");
    return (struct trap_result){x0, x17};
}
__attribute__((noinline)) static void raw_worker_start(void *self, uint32_t port,
        void *stack_low, void *kevent_list, uint32_t flags, int nkevents) {
    uint32_t bits = 0;
    if (self != NULL) bits |= 1U;
    if (port != 0) bits |= 2U;
    if ((uintptr_t)stack_low < (uintptr_t)self) bits |= 4U;
    if (kevent_list == NULL) bits |= 8U;
    if ((flags & 0x00200000U) != 0) bits |= 16U;
    if ((flags & 0x00040000U) != 0) bits |= 32U;
    if ((flags & 0x00004000U) != 0 && (flags & 0xfffU) == 4U) bits |= 64U;
    if (nkevents == 0) bits |= 128U;
    uintptr_t tsd_base = 0;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(tsd_base));
    if (tsd_base == (uintptr_t)self + 224U &&
        *(const uint32_t *)(tsd_base + 3U * sizeof(void *)) == port) bits |= 256U;
    const struct trap_result threadid = thread_self_with_carry_seeded();
    if (!threadid.carry && threadid.value != 0) bits |= 512U;
    atomic_store_explicit(&raw_entry_bits, bits, memory_order_release);
    (void)darwin_syscall(368, 0x4, 0, 0, 0, 0, 0);
    __builtin_trap();
}

#ifndef WORKQ_GUEST_NO_INIT
static void public_libpthread_callback(unsigned long arg) {
    uint64_t tid = 0;
    const int tid_result = pthread_threadid_np(NULL, &tid);
    unsigned bits = 0;
    if (arg == 0x8ffUL) bits |= 1U;
    if (pthread_self() != NULL) bits |= 2U;
    if (tid_result == 0 && tid != 0) bits |= 4U;
    atomic_store_explicit(&callback_argument, arg, memory_order_relaxed);
    atomic_store_explicit(&callback_bits, bits, memory_order_release);
}
#endif

static int run_fixture(void) {
    struct registration_data data = {
        .version = sizeof(data), .dispatch_queue_offset = 160, .main_qos = 0,
        .tsd_offset = 224, .return_to_kernel_offset = 40,
        .mach_thread_self_offset = 24, .stack_addr_hint = 0,
        .mutex_default_policy = 0, .joinable_offset_bits = 392,
        .wq_quantum_expiry_offset = 960,
    };
    // Use the compiler's single arm64e IA/0 function-pointer signature.
    const uintptr_t signed_entry = (uintptr_t)raw_worker_start;
#ifdef WORKQ_GUEST_NO_INIT
    const uint64_t page_size = 16384;
#else
    const uint64_t page_size = (uint64_t)getpagesize();
#endif
    const uint64_t pthread_size = (0x18e0U + page_size - 1U) & ~(page_size - 1U);
    const struct trap_result registration = darwin_syscall(366,
        signed_entry, signed_entry, pthread_size, (uintptr_t)&data,
        sizeof(data), data.dispatch_queue_offset);
    if (!registration.carry && registration.value != 0) return 11;
#ifdef WORKQ_GUEST_NO_INIT
    if (registration.carry) return 12;
#else
    if (registration.carry && registration.value != EINVAL) return 11;
    struct pthread_workqueue_config public_config = {
        .version = PTHREAD_WORKQUEUE_CONFIG_VERSION,
        .workq_cb = public_libpthread_callback,
    };
    if (pthread_workqueue_setup(&public_config, sizeof(public_config)) != 0) return 12;
#endif
    const struct trap_result setup = darwin_syscall(368, 0x400,
        (uintptr_t)&raw_dispatch_config, sizeof(raw_dispatch_config), 0, 0, 0);
    if (setup.carry) return 13;
    const struct trap_result open = darwin_syscall(367, 0, 0, 0, 0, 0, 0);
    if (open.carry) return 14;
    const struct trap_result request = darwin_syscall(368, 0x20, 0, 1, 0x8ff, 0, 0);
    if (request.carry) {
        char message[64];
        const char prefix[] = "request_errno=";
        size_t used = sizeof(prefix) - 1;
        for (size_t i = 0; i < used; ++i) message[i] = prefix[i];
        uint64_t value = request.value;
        char digits[20];
        size_t count = 0;
        do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value != 0);
        while (count != 0) message[used++] = digits[--count];
        message[used++] = ' ';
        const char *registration_state = registration.carry ? "reg=duplicate\n" : "reg=accepted\n";
        for (size_t i = 0; registration_state[i] != '\0'; ++i)
            message[used++] = registration_state[i];
        (void)write(STDOUT_FILENO, message, used);
        return 15;
    }

    const int raw_start = !registration.carry;
    for (unsigned retry = 0; retry < 100000; ++retry) {
#ifdef WORKQ_GUEST_NO_INIT
        const unsigned complete = atomic_load_explicit(&raw_entry_bits, memory_order_acquire);
#else
        const unsigned complete = raw_start
            ? atomic_load_explicit(&raw_entry_bits, memory_order_acquire)
            : atomic_load_explicit(&callback_bits, memory_order_acquire);
#endif
        if (complete != 0) break;
        sched_yield();
    }
    const unsigned raw_result = atomic_load_explicit(&raw_entry_bits, memory_order_acquire);
#ifndef WORKQ_GUEST_NO_INIT
    const unsigned callback_result = atomic_load_explicit(&callback_bits, memory_order_acquire);
    const unsigned long callback_arg = atomic_load_explicit(
        &callback_argument, memory_order_relaxed);
#endif
    if (raw_start) {
        if (raw_result != 1023U) return 16;
#ifdef WORKQ_GUEST_FIXTURE
        static const char message[] = "workq raw worker ABI ok\n";
        return write(STDOUT_FILENO, message, sizeof(message) - 1) ==
            (ssize_t)(sizeof(message) - 1) ? 0 : 17;
#else
        dprintf(STDOUT_FILENO, "register=accepted raw_setup=open=request=ok raw_entry_bits=%u\n",
                raw_result);
        return 0;
#endif
    }
#ifdef WORKQ_GUEST_NO_INIT
    return 18;
#else
    if (registration.value != EINVAL || callback_result != 7U || callback_arg != 0x8ffUL) {
        return 18;
    }
#endif
#ifdef WORKQ_GUEST_FIXTURE
    static const char message[] = "workq libpthread worker callback ok\n";
    return write(STDOUT_FILENO, message, sizeof(message) - 1) ==
        (ssize_t)(sizeof(message) - 1) ? 0 : 19;
#else
#ifdef WORKQ_GUEST_NO_INIT
    dprintf(STDOUT_FILENO, "register=accepted raw_setup=open=request=ok raw_entry_bits=%u\n",
            raw_result);
#else
    dprintf(STDOUT_FILENO,
        "register=EINVAL raw_setup=open=request=ok callback_bits=%u arg=0x%lx\n",
        callback_result, callback_arg);
#endif
    return 0;
#endif
}

int main(void) {
#ifdef WORKQ_GUEST_FIXTURE
    return run_fixture();
#else
    const pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) _exit(run_fixture());
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return 3;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 4;
#endif
}
