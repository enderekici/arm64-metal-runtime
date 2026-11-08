#include <signal.h>
#include <unistd.h>
#include <time.h>

static volatile sig_atomic_t delivered;
static volatile sig_atomic_t nest;

static void secondary(int number) {
    write(STDOUT_FILENO, "nested\n", 7);
}

static void handler(int number) {
    delivered = number;
    write(STDOUT_FILENO, "handler\n", 8);
    if (nest) raise(SIGUSR2);
}

int main(int argc, char **argv) {
    struct sigaction action = {0};
    struct sigaction previous;
    sigset_t blocked = 0;
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &action, &previous) || previous.sa_handler != SIG_DFL) return 1;
    if (argc > 1) {
        const struct timespec wait = {0, 100000000};
        write(STDOUT_FILENO, "ready\n", 6);
        for (int attempt = 0; attempt < 40 && delivered == 0; ++attempt) {
            nanosleep(&wait, 0);
        }
        if (delivered != SIGUSR1) return 6;
        write(STDOUT_FILENO, "resumed\n", 8);
        return 0;
    }
    struct sigaction nested = {0};
    nested.sa_handler = secondary;
    if (sigaction(SIGUSR2, &nested, 0)) return 7;
    nest = 1;
    sigaddset(&blocked, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &blocked, 0) || raise(SIGUSR1) || delivered != 0) return 2;
    write(STDOUT_FILENO, "blocked\n", 8);
    if (sigprocmask(SIG_UNBLOCK, &blocked, 0) || delivered != SIGUSR1) return 3;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGUSR1, &action, &previous) || previous.sa_handler != handler) return 4;
    if (raise(SIGUSR1)) return 5;
    if (signal(SIGUSR1, SIG_DFL) != SIG_IGN) return 8;
    if (signal(SIGUSR1, SIG_IGN) != SIG_DFL) return 9;
    if (sigaction(SIGUSR1, 0, &previous) || !(previous.sa_flags & SA_RESTART)) return 10;
    write(STDOUT_FILENO, "resumed\n", 8);
    return 0;
}
