#include "guest_fork.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace arm64metal::guest_fork {
namespace {

static_assert(sizeof(Header) == 96 && sizeof(int) == 4);
constexpr std::uint64_t version = 0x464f524b00000002ULL;
constexpr std::size_t maxMemory = 512 * 1024 * 1024;
constexpr std::size_t maxAllocations = 65536;
constexpr std::size_t maxDescriptors = 1024;

void require(bool allowed, const char* reason) {
    if (!allowed) throw std::runtime_error(reason);
}

void check(int error) {
    if (error) throw std::system_error(error, std::generic_category(), "Guest fork");
}

void read_all(int fd, void* bytes, std::size_t size) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    while (size) {
        const auto n = ::read(fd, cursor, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) check(errno);
        require(n != 0, "Truncated guest fork snapshot");
        size -= n;
        cursor += n;
    }
}

void write_all(FILE* stream, const void* bytes, std::size_t size) {
    if (std::fwrite(bytes, 1, size, stream) != size) check(errno ? errno : EIO);
}

std::array<unsigned char, 32> digest(const ProcessBootstrapResult& bootstrap) {
    require(bootstrap.memory.size() <= maxMemory, "Guest fork memory exceeds snapshot scope");
    std::array<unsigned char, 32> result{};
    CC_SHA256(bootstrap.memory.data(), static_cast<CC_LONG>(bootstrap.memory.size()), result.data());
    return result;
}

struct Actions {
    posix_spawn_file_actions_t value;
    Actions() { check(posix_spawn_file_actions_init(&value)); }
    ~Actions() { posix_spawn_file_actions_destroy(&value); }
    void inherit(int fd) { check(posix_spawn_file_actions_addinherit_np(&value, fd)); }
};
struct Attributes {
    posix_spawnattr_t value;
    Attributes() { check(posix_spawnattr_init(&value)); }
    ~Attributes() { posix_spawnattr_destroy(&value); }
};
struct AckPipe {
    int ends[2];
    AckPipe() { if (::pipe(ends) < 0) check(errno); }
    ~AckPipe() { for (int fd : ends) if (fd >= 0) ::close(fd); }
    void close_writer() { ::close(ends[1]); ends[1] = -1; }
};

bool wait_for_child_ready(int fd) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(30);
    pollfd event{fd, POLLIN, 0};
    while (steady_clock::now() < deadline) {
        const auto remaining = ceil<milliseconds>(deadline - steady_clock::now()).count();
        if (remaining <= 0) return false;
        const auto n = ::poll(&event, 1, static_cast<int>(remaining));
        if (n < 0 && errno == EINTR) continue;
        if (n != 1 || !(event.revents & POLLIN)) return false;
        char byte = 0;
        const auto received = ::read(fd, &byte, 1);
        if (received < 0 && errno == EINTR) continue;
        return received == 1 && byte == 'R';
    }
    return false;
}

bool pipe_matches(int fd, bool writable) {
    struct stat st{};
    const auto flags = ::fcntl(fd, F_GETFL);
    return fd > 2 && flags >= 0 && ::fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) &&
        (flags & O_ACCMODE) == (writable ? O_WRONLY : O_RDONLY);
}

}  // namespace

Snapshot read_header(int fd, int ready) {
    require(ready != fd && pipe_matches(ready, true), "Invalid guest fork readiness pipe");
    struct stat st{};
    require(fd > 2 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_nlink == 0 && (st.st_mode & 077) == 0 && st.st_uid == ::geteuid(), "Invalid guest fork channel");
    Snapshot snapshot{};
    auto& h = snapshot.header;
    read_all(fd, &h, sizeof h);
    require(h.version == version && h.memoryBytes <= maxMemory &&
            h.allocations <= maxAllocations && h.descriptors <= maxDescriptors &&
            h.registerWords <= maxMemory / sizeof(std::uint64_t), "Invalid guest fork header");
    const auto expectedSize = sizeof h + h.memoryBytes + h.registerWords * 8 +
        h.allocations * 16 + h.descriptors * 8;
    require(st.st_size >= 0 && static_cast<std::uint64_t>(st.st_size) == expectedSize,
            "Invalid guest fork snapshot length");
    // Validate inherited pipe numbers before Metal opens any runtime descriptors.
    if (::lseek(fd, static_cast<off_t>(expectedSize - h.descriptors * 8), SEEK_SET) < 0) check(errno);
    for (std::size_t i = 0; i < h.descriptors; ++i) {
        std::array<int, 2> row{};
        read_all(fd, row.data(), sizeof row);
        require(row[0] != ready && (row[1] == 0 || row[1] == 1) &&
                pipe_matches(row[0], row[1] != 0) && snapshot.descriptors.insert(row[0]).second,
                "Invalid inherited guest fork pipe");
        if (row[1]) snapshot.writable.insert(row[0]);
    }
    if (::lseek(fd, sizeof h, SEEK_SET) < 0) check(errno);
    return snapshot;
}

void restore(int fd, const Snapshot& snapshot, const ProcessBootstrapResult& bootstrap, void* memory,
             std::size_t memoryBytes, std::uint64_t* regs, std::size_t words,
             HostServiceState& state, const std::function<bool(std::uint64_t)>& isContinuation) {
    const auto& h = snapshot.header;
    require(h.memoryBytes == memoryBytes && h.registerWords == words &&
            h.bootstrapDigest == digest(bootstrap), "Guest fork bootstrap identity mismatch");
    std::vector<unsigned char> savedMemory(memoryBytes);
    std::vector<std::uint64_t> saved(words);
    read_all(fd, savedMemory.data(), memoryBytes);
    read_all(fd, saved.data(), words * sizeof(*regs));
    require(words >= guestTokenStart && saved[37] <= 256 &&
            saved[guestTokenCount] <= saved[guestTokenCapacity] &&
            saved[guestTokenCapacity] == (words - guestTokenStart) / 3 &&
            saved[31] >= bootstrap.stack.begin && saved[31] <= bootstrap.stack.end &&
            isContinuation(saved[33]), "Invalid guest fork register state");
    GuestHeap heap({static_cast<std::int64_t>(bootstrap.heap.begin),
                    static_cast<std::int64_t>(bootstrap.heap.end)});
    for (std::size_t i = 0; i < h.allocations; ++i) {
        std::array<std::int64_t, 2> a{};
        read_all(fd, a.data(), sizeof a);
        require(a[0] >= 0 && a[1] > 0 && (a[1] & 15) == 0 &&
                heap.allocate_at(a[0], static_cast<std::uint64_t>(a[1])) == a[0],
                "Invalid guest fork heap allocation");
    }
    auto descriptors = snapshot.descriptors;
    auto writable = snapshot.writable;
    saved[0] = 0;
    saved[34] = 0;
    std::memcpy(memory, savedMemory.data(), memoryBytes);
    std::memcpy(regs, saved.data(), words * sizeof(*regs));
    state.heap = std::move(heap);
    state.openDescriptors = std::move(descriptors);
    state.writableDescriptors = std::move(writable);
}

pid_t spawn(const std::string& executable, const std::vector<std::string>& arguments,
            const ProcessBootstrapResult& bootstrap, const void* memory,
            std::size_t memoryBytes, const std::vector<std::uint64_t>& regs,
            HostServiceState& state) {
    const auto& allocations = state.heap->allocations();
    if (memoryBytes != bootstrap.memory.size() || regs.size() != bootstrap.registers.size() ||
        memoryBytes > maxMemory || allocations.size() > maxAllocations ||
        state.openDescriptors.size() > maxDescriptors) check(ENOTSUP);
    for (int fd = 0; fd < 3; ++fd) if (::fcntl(fd, F_GETFD) != 0) check(ENOTSUP);
    state.spawnedChildren.reserve(state.spawnedChildren.size() + 1);

    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::tmpfile(), &std::fclose);
    if (!file) check(errno);
    Header h{version, memoryBytes, regs.size(), allocations.size(), state.openDescriptors.size(),
             bootstrap.pointerMungeToken, bootstrap.registers[guestPACGAKey],
             bootstrap.registers[guestPACGAKey + 1], digest(bootstrap)};
    write_all(file.get(), &h, sizeof h);
    write_all(file.get(), memory, memoryBytes);
    write_all(file.get(), regs.data(), regs.size() * sizeof(regs[0]));
    for (const auto& entry : allocations) {
        const std::array<std::int64_t, 2> row{entry.first, entry.second};
        write_all(file.get(), row.data(), sizeof row);
    }
    Actions actions;
    for (int fd = 0; fd < 3; ++fd) actions.inherit(fd);
    for (const auto fd : state.openDescriptors) {
        if (fd <= 2) check(ENOTSUP);
        actions.inherit(fd);
        const std::array<int, 2> row{fd, static_cast<int>(state.writableDescriptors.count(fd))};
        write_all(file.get(), row.data(), sizeof row);
    }
    if (std::fflush(file.get()) != 0 || std::fseek(file.get(), 0, SEEK_SET) != 0) check(errno);
    const int snapshot = ::fileno(file.get());
    AckPipe ack;
    actions.inherit(snapshot);
    actions.inherit(ack.ends[1]);
    Attributes attributes;
    check(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_CLOEXEC_DEFAULT));

    std::vector<std::string> args{state.runtimeExecutablePath.string(), "__fork_launch",
        std::to_string(snapshot), std::to_string(ack.ends[1]), executable};
    args.insert(args.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    check(::posix_spawn(&child, argv[0], &actions.value, &attributes.value, argv.data(), environ));
    ack.close_writer();
    if (!wait_for_child_ready(ack.ends[0])) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        check(EAGAIN);
    }
    state.spawnedChildren.push_back(child);
    return child;
}
}  // namespace arm64metal::guest_fork
