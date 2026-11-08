#pragma once

// C++17 Metal compute backend for arm64-metal-runtime.
//
// Build requirement: Apple metal-cpp headers (official distribution:
// https://github.com/apple/metal-cpp) must be on the include path when
// compiling metal_backend.cpp. Link the Metal and Foundation frameworks.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace arm64metal {

struct DispatchSize {
    std::uint64_t x = 1;
    std::uint64_t y = 1;
    std::uint64_t z = 1;
};

class Backend;

class SharedBuffer final {
public:
    SharedBuffer() noexcept;
    ~SharedBuffer();
    SharedBuffer(SharedBuffer&&) noexcept;
    SharedBuffer& operator=(SharedBuffer&&) noexcept;
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;

    void* data() noexcept;
    const void* data() const noexcept;
    std::size_t size() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    explicit SharedBuffer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Backend;
};

class ComputeKernel final {
public:
    ComputeKernel() noexcept;
    ~ComputeKernel();
    ComputeKernel(ComputeKernel&&) noexcept;
    ComputeKernel& operator=(ComputeKernel&&) noexcept;
    ComputeKernel(const ComputeKernel&) = delete;
    ComputeKernel& operator=(const ComputeKernel&) = delete;

    explicit operator bool() const noexcept;

private:
    struct Impl;
    explicit ComputeKernel(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Backend;
};

struct BufferBinding {
    const SharedBuffer* buffer = nullptr;
    std::size_t index = 0;
    std::size_t offset = 0;
};

struct DispatchTiming {
    // Host wall time includes command submission and completion wait.
    double cpuSeconds = 0.0;
    // Device timestamp interval; zero when Metal does not report timestamps.
    double gpuSeconds = 0.0;
};

class Backend final {
public:
    static Backend createDefault();

    Backend() noexcept;
    ~Backend();
    Backend(Backend&&) noexcept;
    Backend& operator=(Backend&&) noexcept;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    ComputeKernel compileCompute(std::string_view source,
                                 std::string_view functionName) const;
    SharedBuffer makeSharedBuffer(std::size_t byteLength) const;
    [[nodiscard]] std::size_t maxBufferLength() const noexcept;
    [[nodiscard]] std::uint64_t compiledPipelineCount() const noexcept;
    [[nodiscard]] std::uint64_t completedDispatchCount() const noexcept;

    // `grid` is the exact thread count. `threadsPerGroup` is the requested
    // threadgroup shape; dispatchThreads handles a partial final group.
    DispatchTiming dispatch(const ComputeKernel& kernel,
                            const std::vector<BufferBinding>& buffers,
                            DispatchSize grid,
                            DispatchSize threadsPerGroup) const;

    const char* deviceName() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    explicit Backend(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace arm64metal
