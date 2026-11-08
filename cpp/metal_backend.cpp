#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "metal_backend.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace arm64metal {
namespace {

class AutoreleasePool final {
public:
    AutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {
        if (pool_ == nullptr) {
            throw std::runtime_error("Unable to create Foundation autorelease pool");
        }
    }
    ~AutoreleasePool() { pool_->drain(); }
    AutoreleasePool(const AutoreleasePool&) = delete;
    AutoreleasePool& operator=(const AutoreleasePool&) = delete;

private:
    NS::AutoreleasePool* pool_;
};

std::string errorMessage(const char* operation, NS::Error* error) {
    std::string message(operation);
    if (error != nullptr) {
        NS::String* description = error->localizedDescription();
        if (description != nullptr && description->utf8String() != nullptr) {
            message += ": ";
            message += description->utf8String();
        }
    }
    return message;
}

NS::UInteger checkedSize(std::uint64_t value, const char* label) {
    if (value == 0 || value > std::numeric_limits<NS::UInteger>::max()) {
        throw std::invalid_argument(std::string(label) + " must be nonzero and fit NS::UInteger");
    }
    return static_cast<NS::UInteger>(value);
}

} // namespace

struct Backend::Impl {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
    std::string name;

    ~Impl() {
        if (queue != nullptr) queue->release();
        if (device != nullptr) device->release();
    }
};

struct SharedBuffer::Impl {
    MTL::Buffer* buffer = nullptr;
    std::size_t byteLength = 0;

    ~Impl() {
        if (buffer != nullptr) buffer->release();
    }
};

struct ComputeKernel::Impl {
    MTL::ComputePipelineState* pipeline = nullptr;

    ~Impl() {
        if (pipeline != nullptr) pipeline->release();
    }
};

SharedBuffer::SharedBuffer() noexcept = default;
SharedBuffer::SharedBuffer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SharedBuffer::~SharedBuffer() = default;
SharedBuffer::SharedBuffer(SharedBuffer&&) noexcept = default;
SharedBuffer& SharedBuffer::operator=(SharedBuffer&&) noexcept = default;

void* SharedBuffer::data() noexcept {
    return impl_ == nullptr ? nullptr : impl_->buffer->contents();
}

const void* SharedBuffer::data() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->buffer->contents();
}

std::size_t SharedBuffer::size() const noexcept {
    return impl_ == nullptr ? 0 : impl_->byteLength;
}

SharedBuffer::operator bool() const noexcept {
    return impl_ != nullptr && impl_->buffer != nullptr;
}

ComputeKernel::ComputeKernel() noexcept = default;
ComputeKernel::ComputeKernel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ComputeKernel::~ComputeKernel() = default;
ComputeKernel::ComputeKernel(ComputeKernel&&) noexcept = default;
ComputeKernel& ComputeKernel::operator=(ComputeKernel&&) noexcept = default;

ComputeKernel::operator bool() const noexcept {
    return impl_ != nullptr && impl_->pipeline != nullptr;
}

Backend::Backend() noexcept = default;
Backend::Backend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Backend::~Backend() = default;
Backend::Backend(Backend&&) noexcept = default;
Backend& Backend::operator=(Backend&&) noexcept = default;

Backend Backend::createDefault() {
    AutoreleasePool pool;
    std::unique_ptr<Impl> impl(new Impl());
    impl->device = MTL::CreateSystemDefaultDevice();
    if (impl->device == nullptr) {
        throw std::runtime_error("No default Metal device is available");
    }
    impl->queue = impl->device->newCommandQueue();
    if (impl->queue == nullptr) {
        throw std::runtime_error("Unable to create a Metal command queue");
    }
    NS::String* name = impl->device->name();
    impl->name = (name != nullptr && name->utf8String() != nullptr) ? name->utf8String() : "Metal device";
    return Backend(std::move(impl));
}

ComputeKernel Backend::compileCompute(std::string_view source,
                                      std::string_view functionName) const {
    if (impl_ == nullptr || impl_->device == nullptr) {
        throw std::logic_error("Metal backend is not initialized");
    }
    if (source.empty() || functionName.empty()) {
        throw std::invalid_argument("Metal source and compute function name must be nonempty");
    }
    AutoreleasePool pool;
    const std::string sourceString(source);
    const std::string functionString(functionName);
    NS::Error* error = nullptr;
    NS::String* msl = NS::String::string(sourceString.c_str(), NS::UTF8StringEncoding);
    MTL::Library* library = impl_->device->newLibrary(msl, nullptr, &error);
    if (library == nullptr) {
        throw std::runtime_error(errorMessage("Metal shader compilation failed", error));
    }
    NS::String* entryPoint = NS::String::string(functionString.c_str(), NS::UTF8StringEncoding);
    MTL::Function* function = library->newFunction(entryPoint);
    if (function == nullptr) {
        library->release();
        throw std::runtime_error("Metal library does not contain compute function: " + functionString);
    }
    MTL::ComputePipelineState* pipeline = impl_->device->newComputePipelineState(function, &error);
    function->release();
    library->release();
    if (pipeline == nullptr) {
        throw std::runtime_error(errorMessage("Metal compute pipeline creation failed", error));
    }
    std::unique_ptr<ComputeKernel::Impl> kernel(new ComputeKernel::Impl());
    kernel->pipeline = pipeline;
    return ComputeKernel(std::move(kernel));
}

SharedBuffer Backend::makeSharedBuffer(std::size_t byteLength) const {
    if (impl_ == nullptr || impl_->device == nullptr) {
        throw std::logic_error("Metal backend is not initialized");
    }
    if (byteLength == 0 || byteLength > std::numeric_limits<NS::UInteger>::max()) {
        throw std::invalid_argument("Metal shared buffer size must be nonzero and fit NS::UInteger");
    }
    AutoreleasePool pool;
    MTL::Buffer* buffer = impl_->device->newBuffer(
        static_cast<NS::UInteger>(byteLength), MTL::ResourceStorageModeShared);
    if (buffer == nullptr) {
        throw std::runtime_error("Metal failed to allocate shared buffer");
    }
    std::unique_ptr<SharedBuffer::Impl> result(new SharedBuffer::Impl());
    result->buffer = buffer;
    result->byteLength = byteLength;
    return SharedBuffer(std::move(result));
}

std::size_t Backend::maxBufferLength() const noexcept {
    if (impl_ == nullptr || impl_->device == nullptr) return 0;
    return static_cast<std::size_t>(impl_->device->maxBufferLength());
}

DispatchTiming Backend::dispatch(const ComputeKernel& kernel,
                                 const std::vector<BufferBinding>& buffers,
                                 DispatchSize grid,
                                 DispatchSize threadsPerGroup) const {
    if (impl_ == nullptr || impl_->queue == nullptr) {
        throw std::logic_error("Metal backend is not initialized");
    }
    if (kernel.impl_ == nullptr || kernel.impl_->pipeline == nullptr) {
        throw std::invalid_argument("Compute kernel is not initialized");
    }
    const MTL::Size gridSize = MTL::Size::Make(checkedSize(grid.x, "grid.x"),
                                                checkedSize(grid.y, "grid.y"),
                                                checkedSize(grid.z, "grid.z"));
    const MTL::Size groupSize = MTL::Size::Make(checkedSize(threadsPerGroup.x, "threadsPerGroup.x"),
                                                 checkedSize(threadsPerGroup.y, "threadsPerGroup.y"),
                                                 checkedSize(threadsPerGroup.z, "threadsPerGroup.z"));
    if (threadsPerGroup.x > std::numeric_limits<std::uint64_t>::max() / threadsPerGroup.y ||
        threadsPerGroup.x * threadsPerGroup.y >
            std::numeric_limits<std::uint64_t>::max() / threadsPerGroup.z) {
        throw std::invalid_argument("Threadgroup size overflows uint64");
    }
    const std::uint64_t threadCount = threadsPerGroup.x * threadsPerGroup.y * threadsPerGroup.z;
    if (threadCount > kernel.impl_->pipeline->maxTotalThreadsPerThreadgroup()) {
        throw std::invalid_argument("Requested threadgroup exceeds the pipeline thread limit");
    }
    for (const BufferBinding& binding : buffers) {
        if (binding.buffer == nullptr || !static_cast<bool>(*binding.buffer)) {
            throw std::invalid_argument("Every Metal buffer binding must reference a live shared buffer");
        }
        if (binding.offset > binding.buffer->size()) {
            throw std::out_of_range("Metal buffer binding offset exceeds buffer length");
        }
        if (binding.index > std::numeric_limits<NS::UInteger>::max() ||
            binding.offset > std::numeric_limits<NS::UInteger>::max()) {
            throw std::out_of_range("Metal buffer binding index or offset exceeds NS::UInteger");
        }
    }

    AutoreleasePool pool;
    MTL::CommandBuffer* command = impl_->queue->commandBuffer();
    if (command == nullptr) {
        throw std::runtime_error("Metal failed to allocate a command buffer");
    }
    MTL::ComputeCommandEncoder* encoder = command->computeCommandEncoder();
    if (encoder == nullptr) {
        throw std::runtime_error("Metal failed to create a compute encoder");
    }
    encoder->setComputePipelineState(kernel.impl_->pipeline);
    for (const BufferBinding& binding : buffers) {
        encoder->setBuffer(binding.buffer->impl_->buffer,
                            static_cast<NS::UInteger>(binding.offset),
                            static_cast<NS::UInteger>(binding.index));
    }
    const auto cpuStart = std::chrono::steady_clock::now();
    encoder->dispatchThreads(gridSize, groupSize);
    encoder->endEncoding();
    command->commit();
    command->waitUntilCompleted();
    const auto cpuEnd = std::chrono::steady_clock::now();
    if (command->status() != MTL::CommandBufferStatusCompleted) {
        std::string message = errorMessage("Metal command execution failed", command->error());
        throw std::runtime_error(message);
    }
    const double gpuStart = command->GPUStartTime();
    const double gpuEnd = command->GPUEndTime();
    DispatchTiming timing;
    timing.cpuSeconds = std::chrono::duration<double>(cpuEnd - cpuStart).count();
    timing.gpuSeconds = gpuStart > 0.0 && gpuEnd >= gpuStart ? gpuEnd - gpuStart : 0.0;
    return timing;
}

const char* Backend::deviceName() const noexcept {
    return impl_ == nullptr ? "" : impl_->name.c_str();
}

Backend::operator bool() const noexcept {
    return impl_ != nullptr && impl_->device != nullptr && impl_->queue != nullptr;
}

} // namespace arm64metal
