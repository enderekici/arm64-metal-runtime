#include "guest_survey.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace arm64metal::guest_survey {
namespace {

int open_append(const char* path) {
    if (path == nullptr || *path == '\0') return STDERR_FILENO;
    const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    return fd < 0 ? STDERR_FILENO : fd;
}

struct Sink {
    bool on = false;
    int fd = STDERR_FILENO;
};

const Sink& survey_sink() {
    static const Sink sink = [] {
        Sink result;
        const char* flag = std::getenv("ARM64_METAL_SURVEY");
        result.on = flag != nullptr && std::strcmp(flag, "1") == 0;
        if (result.on) result.fd = open_append(std::getenv("ARM64_METAL_SURVEY_LOG"));
        return result;
    }();
    return sink;
}

const Sink& boundary_sink() {
    static const Sink sink = [] {
        Sink result;
        const char* path = std::getenv("ARM64_METAL_BOUNDARY_TRACE");
        result.on = path != nullptr && *path != '\0';
        if (result.on) result.fd = open_append(path);
        return result;
    }();
    return sink;
}

void write_line(int fd, const std::string& line) {
    std::size_t done = 0;
    while (done < line.size()) {
        const auto written = ::write(fd, line.data() + done, line.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        done += static_cast<std::size_t>(written);
    }
}

std::string hex(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
    return buffer;
}

std::string text(std::string_view value) {
    return value.empty() ? std::string("-") : std::string(value);
}

}  // namespace

bool enabled() noexcept { return survey_sink().on; }

bool boundary_enabled() noexcept { return boundary_sink().on; }

void note(std::string_view kind, std::int64_t num, std::string_view sub, std::uint64_t pc,
          std::uint64_t lr, std::uint64_t thread, std::string_view detail) {
    const auto& sink = survey_sink();
    if (!sink.on) return;
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::string key = std::string(kind) + '|' + std::to_string(num) + '|' + std::string(sub);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(std::move(key)).second) return;
    }
    std::string line = "SURVEY kind=" + text(kind) + " num=" + std::to_string(num) +
        " sub=" + text(sub) + " pc=" + hex(pc) + " lr=" + hex(lr) +
        " thread=" + std::to_string(thread);
    if (!detail.empty()) line += ' ' + std::string(detail);
    line += '\n';
    write_line(sink.fd, line);
}

void boundary(std::string_view kind, std::int64_t num, std::string_view sub,
              const std::uint64_t* args, std::size_t count, std::uint64_t pc,
              std::uint64_t lr, std::uint64_t thread) {
    const auto& sink = boundary_sink();
    if (!sink.on) return;
    std::string line = "BOUNDARY kind=" + text(kind) + " num=" + std::to_string(num) +
        " sub=" + text(sub) + " pc=" + hex(pc) + " lr=" + hex(lr) +
        " thread=" + std::to_string(thread);
    for (std::size_t index = 0; index < count; ++index) {
        line += " x" + std::to_string(index) + '=' + hex(args[index]);
    }
    line += '\n';
    write_line(sink.fd, line);
}

}  // namespace arm64metal::guest_survey
