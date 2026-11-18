#pragma once

#include "process_host.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace arm64metal::guest_fork {

struct Header {
    std::uint64_t version, memoryBytes, registerWords, allocations, descriptors;
    std::uint64_t pointerMungeToken, pacKey0, pacKey1;
    std::array<unsigned char, 32> bootstrapDigest;
};

// A fresh launcher reads only a private, unlinked snapshot inherited from its parent.
struct Snapshot {
    Header header;
    std::set<int> descriptors;
    std::set<int> writable;
};
Snapshot read_header(int fd, int ready);

void restore(int fd, const Snapshot& snapshot, const ProcessBootstrapResult& bootstrap,
             void* memory, std::size_t memoryBytes, std::uint64_t* registers,
             std::size_t registerWords, HostServiceState& state,
             const std::function<bool(std::uint64_t)>& isContinuation);

// Returns a real child pid only after it acknowledges restoring the continuation.
pid_t spawn(const std::string& executable, const std::vector<std::string>& arguments,
            const ProcessBootstrapResult& bootstrap, const void* memory,
            std::size_t memoryBytes, const std::vector<std::uint64_t>& registers,
            HostServiceState& state);

}  // namespace arm64metal::guest_fork
