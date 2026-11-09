#include "process_host.hpp"
#include "host_service_registry.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace arm64metal;

int main() {
  char directory[] = "/tmp/arm64-metal-file-metadata.XXXXXX";
  assert(::mkdtemp(directory) != nullptr);
  const auto path = std::string(directory) + "/payload";
  const auto descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  const auto directory_descriptor = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(descriptor >= 0);
  assert(directory_descriptor >= 0);

  std::vector<std::uint8_t> bytes(2048);
  GuestMemory memory(bytes);
  ProcessBootstrapResult bootstrap{};
  bootstrap.errnoSlot = 32;
  HostServiceState state;
  state.openDescriptors.insert(descriptor);
  state.openDescriptors.insert(directory_descriptor);

  const auto put_string = [&](std::uint64_t address, const std::string& value) {
    auto* output = memory.buffer(address, value.size() + 1, true);
    std::memcpy(output, value.c_str(), value.size() + 1);
  };
  const auto call = [&](std::uint64_t service, std::uint64_t a0, std::uint64_t a1,
                        std::uint64_t a2 = 0, std::uint64_t a3 = 0) {
    std::vector<std::uint64_t> registers(36);
    registers[0] = a0;
    registers[1] = a1;
    registers[2] = a2;
    registers[3] = a3;
    (void)dispatch_guest_host_service(registers, memory, bootstrap, state, service);
    return registers[0];
  };
  const auto guest_errno = [&] {
    std::uint32_t value = 0;
    std::memcpy(&value, memory.buffer(bootstrap.errnoSlot, sizeof(value)), sizeof(value));
    return static_cast<int>(value);
  };
  struct GuestTimespec { std::int64_t seconds; std::int64_t nanoseconds; };
  static_assert(sizeof(GuestTimespec) == 16);
  static_assert(offsetof(GuestTimespec, nanoseconds) == 8);
  const auto store_times = [&](std::uint64_t address, std::int64_t seconds) {
    const GuestTimespec times[2]{{seconds, 123456000}, {seconds + 1, 789000000}};
    std::memcpy(memory.buffer(address, sizeof(times), true), times, sizeof(times));
  };
  struct stat info{};

  put_string(128, path);
  assert(call(240, 128, 0640) == 0);
  assert(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
  assert(call(241, static_cast<std::uint64_t>(descriptor), 0600) == 0);
  assert(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);

  store_times(512, 1577934245);
  assert(call(243, static_cast<std::uint64_t>(AT_FDCWD), 128, 512, 0) == 0);
  assert(::stat(path.c_str(), &info) == 0);
  assert(info.st_atimespec.tv_sec == 1577934245 && info.st_atimespec.tv_nsec == 123456000);
  assert(info.st_mtimespec.tv_sec == 1577934246 && info.st_mtimespec.tv_nsec == 789000000);

  put_string(256, "payload");
  store_times(512, 1577934600);
  assert(call(243, static_cast<std::uint64_t>(directory_descriptor), 256, 512,
              AT_SYMLINK_NOFOLLOW) == 0);
  assert(::stat(path.c_str(), &info) == 0);
  assert(info.st_atimespec.tv_sec == 1577934600 && info.st_atimespec.tv_nsec == 123456000);
  assert(info.st_mtimespec.tv_sec == 1577934601 && info.st_mtimespec.tv_nsec == 789000000);

  store_times(512, 1577935000);
  assert(call(242, static_cast<std::uint64_t>(descriptor), 512) == 0);
  assert(::stat(path.c_str(), &info) == 0);
  assert(info.st_atimespec.tv_sec == 1577935000 && info.st_atimespec.tv_nsec == 123456000);
  assert(info.st_mtimespec.tv_sec == 1577935001 && info.st_mtimespec.tv_nsec == 789000000);

  assert(call(241, static_cast<std::uint64_t>(descriptor + 1000), 0600) == UINT64_MAX);
  assert(guest_errno() == EBADF);
  assert(call(243, static_cast<std::uint64_t>(AT_FDCWD), 128, 512, 1) == UINT64_MAX);
  assert(guest_errno() == EINVAL);

  assert(::unlink(path.c_str()) == 0);
  assert(::rmdir(directory) == 0);
  return 0;
}
