#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "metal_backend.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace arm64metal {
struct KernelCache;
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

std::uint64_t fnv1a(std::uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex16(std::uint64_t hash) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::string sha256Hex(const std::string& data, const std::string& salt) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, salt.data(), static_cast<CC_LONG>(salt.size()));
    CC_SHA256_Update(&context, "\0", 1);
    CC_SHA256_Update(&context, data.data(), static_cast<CC_LONG>(data.size()));
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    std::string text;
    char byte[3];
    for (unsigned char value : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        text += byte;
    }
    return text;
}

std::string osBuild() {
    char buffer[128] = {0};
    std::size_t size = sizeof(buffer) - 1;
    if (sysctlbyname("kern.osversion", buffer, &size, nullptr, 0) != 0) return "unknown";
    return buffer;
}

// Identity of the compiled-pipeline archives: OS build and GPU only. Metal keys
// archive entries by function and pipeline descriptor, so a changed source
// simply misses; host-only edits and regenerated MSL need no new directory.
std::string cacheIdentity(const std::string& gpuName) {
    std::uint64_t hash = 14695981039346656037ull;
    const std::string os = osBuild();
    hash = fnv1a(hash, os.data(), os.size());
    hash = fnv1a(hash, gpuName.data(), gpuName.size());
    return hex16(hash);
}

std::string toolPath(const char* tool) {
    FILE* pipe = nullptr;
    std::string command = std::string("/usr/bin/xcrun -sdk macosx -f ") + tool + " 2>/dev/null";
    pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return "";
    char line[4096] = {0};
    std::string path;
    if (std::fgets(line, sizeof(line), pipe) != nullptr) path = line;
    pclose(pipe);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (access(path.c_str(), X_OK) != 0) return "";
    return path;
}

bool runTool(const std::vector<std::string>& args, const std::string& logPath) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ARM64_METAL_DUMP_MSL=<dir> writes every compiled source to <dir>/<pid>-<seq>.metal.
void dumpSource(const std::string& source) {
    static const char* directory = std::getenv("ARM64_METAL_DUMP_MSL");
    static std::atomic<unsigned> sequence{0};
    if (directory == nullptr || directory[0] == '\0') return;
    mkdir(directory, 0755);
    char name[64];
    std::snprintf(name, sizeof(name), "/%d-%04u.metal", getpid(), sequence.fetch_add(1));
    if (FILE* out = std::fopen((std::string(directory) + name).c_str(), "wb")) {
        std::fwrite(source.data(), 1, source.size(), out);
        std::fclose(out);
    }
}

constexpr unsigned flushEveryNewEntries = 32;

std::mutex activeCachesMutex;
std::vector<KernelCache*> activeCaches;

void flushActiveCaches();

} // namespace

// Persistent compiled-kernel cache (ARM64_METAL_KERNEL_CACHE=<dir>). Existing
// archives of this runtime build are read-only lookup sources; new pipelines go
// to a per-process archive written atomically to <key>.<pid>.bin, so concurrent
// processes never write the same file. Metal keys archive entries by the
// compiled function and pipeline descriptor, and the file name carries the
// runtime binary identity.
struct KernelCache {
    MTL::Device* device = nullptr;
    std::string directory;
    std::string key;
    std::vector<MTL::BinaryArchive*> readers;
    MTL::BinaryArchive* writer = nullptr;
    std::mutex mutex;
    unsigned dirty = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::string libraryDirectory;
    std::string librarySalt;
    bool libraryEnabled = false;
    std::atomic<std::uint64_t> libraryHits{0};
    std::atomic<std::uint64_t> libraryMisses{0};
    std::atomic<std::uint64_t> libraryFallbacks{0};
    std::atomic<std::uint64_t> libraryCounter{0};
    std::mutex toolMutex;
    bool toolsResolved = false;
    std::string metalTool;
    std::string metallibTool;

    static NS::String* nsString(const std::string& text) {
        return NS::String::string(text.c_str(), NS::UTF8StringEncoding);
    }

    static std::unique_ptr<KernelCache> open(MTL::Device* device) {
        const char* dir = std::getenv("ARM64_METAL_KERNEL_CACHE");
        if (dir == nullptr || dir[0] == '\0') return nullptr;
        std::unique_ptr<KernelCache> cache(new KernelCache());
        cache->device = device;
        cache->directory = dir;
        mkdir(dir, 0755);
        struct stat info;
        if (stat(dir, &info) != 0 || !S_ISDIR(info.st_mode)) return nullptr;
        {
            NS::String* gpu = device->name();
            cache->key = cacheIdentity(gpu != nullptr && gpu->utf8String() != nullptr
                                           ? gpu->utf8String() : "");
        }
        cache->libraryDirectory = cache->directory + "/lib";
        mkdir(cache->libraryDirectory.c_str(), 0755);
        cache->libraryEnabled = std::getenv("ARM64_METAL_NO_LIBRARY_STORE") == nullptr;
        const std::string prefix = cache->key + ".";
        if (DIR* handle = opendir(dir)) {
            while (dirent* entry = readdir(handle)) {
                const std::string name = entry->d_name;
                if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
                    name.compare(name.size() - 4, 4, ".bin") != 0) {
                    continue;
                }
                MTL::BinaryArchiveDescriptor* descriptor = MTL::BinaryArchiveDescriptor::alloc()->init();
                descriptor->setUrl(NS::URL::fileURLWithPath(nsString(cache->directory + "/" + name)));
                NS::Error* error = nullptr;
                MTL::BinaryArchive* archive = device->newBinaryArchive(descriptor, &error);
                descriptor->release();
                if (archive != nullptr) cache->readers.push_back(archive);
            }
            closedir(handle);
        }
        MTL::BinaryArchiveDescriptor* fresh = MTL::BinaryArchiveDescriptor::alloc()->init();
        NS::Error* error = nullptr;
        cache->writer = device->newBinaryArchive(fresh, &error);
        fresh->release();
        if (cache->writer == nullptr) return nullptr;
        {
            std::lock_guard<std::mutex> lock(activeCachesMutex);
            if (activeCaches.empty()) std::atexit(flushActiveCaches);
            activeCaches.push_back(cache.get());
        }
        return cache;
    }

    ~KernelCache() {
        {
            std::lock_guard<std::mutex> lock(activeCachesMutex);
            for (auto it = activeCaches.begin(); it != activeCaches.end(); ++it) {
                if (*it == this) {
                    activeCaches.erase(it);
                    break;
                }
            }
        }
        flush();
        for (auto* reader : readers) reader->release();
        if (writer != nullptr) writer->release();
    }

    // Caller holds `mutex`.
    void flushLocked() {
        if (dirty == 0 || writer == nullptr) return;
        const std::string final = directory + "/" + key + "." + std::to_string(getpid()) + ".bin";
        const std::string temp = final + ".tmp";
        NS::Error* error = nullptr;
        if (writer->serializeToURL(NS::URL::fileURLWithPath(nsString(temp)), &error)) {
            if (std::rename(temp.c_str(), final.c_str()) == 0) {
                dirty = 0;
                return;
            }
        }
        std::remove(temp.c_str());
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex);
        flushLocked();
    }

    bool resolveTools() {
        std::lock_guard<std::mutex> lock(toolMutex);
        if (!toolsResolved) {
            toolsResolved = true;
            metalTool = toolPath("metal");
            metallibTool = toolPath("metallib");
            librarySalt = osBuild() + "|" + metalTool + "|" + metallibTool +
                          "|offline-default-std-default-fastmath";
        }
        return !metalTool.empty() && !metallibTool.empty();
    }

    // Content-addressed library store: <dir>/lib/<sha256(source)>.metallib. A hit
    // loads the stored library and skips the Metal front end. A miss compiles the
    // source offline (metal -c, metallib), publishes with tmp+rename so concurrent
    // readers see either nothing or a complete file, and loads that. Returns
    // nullptr when the store cannot serve the source; the caller then compiles
    // in process.
    MTL::Library* loadStoredLibrary(const std::string& source) {
        if (!libraryEnabled) return nullptr;
        resolveTools();
        const std::string name = sha256Hex(source, librarySalt);
        const std::string final = libraryDirectory + "/" + name + ".metallib";
        auto load = [&]() -> MTL::Library* {
            struct stat info;
            if (stat(final.c_str(), &info) != 0 || info.st_size == 0) return nullptr;
            NS::Error* error = nullptr;
            return device->newLibrary(NS::URL::fileURLWithPath(nsString(final)), &error);
        };
        if (MTL::Library* library = load()) {
            ++libraryHits;
            return library;
        }
        if (!resolveTools()) {
            ++libraryFallbacks;
            return nullptr;
        }
        const std::string stem = libraryDirectory + "/" + name + "." + std::to_string(getpid()) +
                                 "." + std::to_string(libraryCounter.fetch_add(1));
        const std::string metalFile = stem + ".metal";
        const std::string airFile = stem + ".air";
        const std::string tempLibrary = stem + ".metallib";
        const std::string logFile = stem + ".log";
        bool ok = false;
        if (FILE* out = std::fopen(metalFile.c_str(), "wb")) {
            ok = std::fwrite(source.data(), 1, source.size(), out) == source.size();
            ok = (std::fclose(out) == 0) && ok;
        }
        ok = ok && runTool({metalTool, "-c", metalFile, "-o", airFile}, logFile) &&
             runTool({metallibTool, airFile, "-o", tempLibrary}, logFile) &&
             std::rename(tempLibrary.c_str(), final.c_str()) == 0;
        std::remove(metalFile.c_str());
        std::remove(airFile.c_str());
        std::remove(tempLibrary.c_str());
        if (ok) std::remove(logFile.c_str());
        if (ok) {
            if (MTL::Library* library = load()) {
                ++libraryMisses;
                return library;
            }
        }
        ++libraryFallbacks;
        return nullptr;
    }

    NS::Array* archiveArray() {
        std::vector<NS::Object*> all(readers.begin(), readers.end());
        all.push_back(writer);
        return NS::Array::array(all.data(), all.size());
    }

    // Returns a pipeline from the cache or nullptr on a miss.
    MTL::ComputePipelineState* lookup(MTL::ComputePipelineDescriptor* descriptor) {
        std::lock_guard<std::mutex> lock(mutex);
        descriptor->setBinaryArchives(archiveArray());
        NS::Error* error = nullptr;
        MTL::ComputePipelineState* pipeline = device->newComputePipelineState(
            descriptor, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr, &error);
        if (pipeline != nullptr) ++hits;
        return pipeline;
    }

    // Compiles into the per-process archive and returns the pipeline built from it.
    MTL::ComputePipelineState* insert(MTL::ComputePipelineDescriptor* descriptor, NS::Error** error) {
        std::lock_guard<std::mutex> lock(mutex);
        ++misses;
        if (writer->addComputePipelineFunctions(descriptor, error)) {
            if (++dirty >= flushEveryNewEntries) flushLocked();
        }
        descriptor->setBinaryArchives(archiveArray());
        return device->newComputePipelineState(descriptor, MTL::PipelineOptionNone, nullptr, error);
    }
};

namespace {
void flushActiveCaches() {
    std::vector<KernelCache*> caches;
    {
        std::lock_guard<std::mutex> lock(activeCachesMutex);
        caches = activeCaches;
    }
    for (auto* cache : caches) cache->flush();
}
} // namespace

struct Backend::Impl {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
    std::string name;
    std::atomic<std::uint64_t> compiledPipelines{0};
    std::atomic<std::uint64_t> sourceLibraryCompiles{0};
    std::atomic<std::uint64_t> completedDispatches{0};
    std::unique_ptr<KernelCache> kernelCache;
    std::atomic<std::uint64_t> libraryMicros{0};
    std::atomic<std::uint64_t> pipelineMicros{0};

    ~Impl() {
        if (kernelCache != nullptr) {
            const char* perf = std::getenv("ARM64_METAL_PERF_SUMMARY");
            if (perf != nullptr && std::strcmp(perf, "1") == 0) {
                std::fprintf(stderr,
                             "arm64-metal kernel-cache hits=%llu misses=%llu readers=%zu "
                             "library_hits=%llu library_misses=%llu library_fallbacks=%llu "
                             "library_s=%.3f pipeline_s=%.3f\n",
                             static_cast<unsigned long long>(kernelCache->hits),
                             static_cast<unsigned long long>(kernelCache->misses),
                             kernelCache->readers.size(),
                             static_cast<unsigned long long>(kernelCache->libraryHits.load()),
                             static_cast<unsigned long long>(kernelCache->libraryMisses.load()),
                             static_cast<unsigned long long>(kernelCache->libraryFallbacks.load()),
                             libraryMicros.load() / 1e6, pipelineMicros.load() / 1e6);
            }
            kernelCache.reset();
        }
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

std::uint64_t ComputeKernel::threadExecutionWidth() const noexcept {
    return impl_ == nullptr || impl_->pipeline == nullptr
               ? 0
               : static_cast<std::uint64_t>(impl_->pipeline->threadExecutionWidth());
}

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
    impl->kernelCache = KernelCache::open(impl->device);
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
    impl_->sourceLibraryCompiles.fetch_add(1, std::memory_order_relaxed);
    const auto libraryStarted = std::chrono::steady_clock::now();
    dumpSource(sourceString);
    MTL::Library* library = impl_->kernelCache != nullptr
                                ? impl_->kernelCache->loadStoredLibrary(sourceString) : nullptr;
    if (library == nullptr) library = impl_->device->newLibrary(msl, nullptr, &error);
    impl_->libraryMicros.fetch_add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - libraryStarted).count()), std::memory_order_relaxed);
    if (library == nullptr) {
        throw std::runtime_error(errorMessage("Metal shader compilation failed", error));
    }
    NS::String* entryPoint = NS::String::string(functionString.c_str(), NS::UTF8StringEncoding);
    MTL::Function* function = library->newFunction(entryPoint);
    if (function == nullptr) {
        library->release();
        throw std::runtime_error("Metal library does not contain compute function: " + functionString);
    }
    const auto pipelineStarted = std::chrono::steady_clock::now();
    MTL::ComputePipelineState* pipeline = nullptr;
    if (impl_->kernelCache != nullptr) {
        MTL::ComputePipelineDescriptor* descriptor = MTL::ComputePipelineDescriptor::alloc()->init();
        descriptor->setComputeFunction(function);
        pipeline = impl_->kernelCache->lookup(descriptor);
        if (pipeline == nullptr) pipeline = impl_->kernelCache->insert(descriptor, &error);
        descriptor->release();
    } else {
        pipeline = impl_->device->newComputePipelineState(function, &error);
    }
    impl_->pipelineMicros.fetch_add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pipelineStarted).count()), std::memory_order_relaxed);
    function->release();
    library->release();
    if (pipeline == nullptr) {
        throw std::runtime_error(errorMessage("Metal compute pipeline creation failed", error));
    }
    impl_->compiledPipelines.fetch_add(1, std::memory_order_relaxed);
    std::unique_ptr<ComputeKernel::Impl> kernel(new ComputeKernel::Impl());
    kernel->pipeline = pipeline;
    return ComputeKernel(std::move(kernel));
}

ComputeKernel Backend::loadComputeLibrary(std::string_view libraryPath,
                                          std::string_view functionName) const {
    if (impl_ == nullptr || impl_->device == nullptr) {
        throw std::logic_error("Metal backend is not initialized");
    }
    if (libraryPath.empty() || functionName.empty()) {
        throw std::invalid_argument("Metal library path and compute function name must be nonempty");
    }
    AutoreleasePool pool;
    const std::string pathString(libraryPath);
    const std::string functionString(functionName);
    NS::Error* error = nullptr;
    NS::String* path = NS::String::string(pathString.c_str(), NS::UTF8StringEncoding);
    NS::URL* url = NS::URL::fileURLWithPath(path);
    if (url == nullptr) throw std::runtime_error("Unable to form Metal library file URL");
    MTL::Library* library = impl_->device->newLibrary(url, &error);
    if (library == nullptr) {
        throw std::runtime_error(errorMessage("Metal library load failed", error));
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
    impl_->compiledPipelines.fetch_add(1, std::memory_order_relaxed);
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

std::uint64_t Backend::compiledPipelineCount() const noexcept {
    return impl_ == nullptr ? 0 :
        impl_->compiledPipelines.load(std::memory_order_relaxed);
}

std::uint64_t Backend::sourceLibraryCompileCount() const noexcept {
    return impl_ == nullptr ? 0 :
        impl_->sourceLibraryCompiles.load(std::memory_order_relaxed);
}

std::uint64_t Backend::completedDispatchCount() const noexcept {
    return impl_ == nullptr ? 0 :
        impl_->completedDispatches.load(std::memory_order_relaxed);
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
    impl_->completedDispatches.fetch_add(1, std::memory_order_relaxed);
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
