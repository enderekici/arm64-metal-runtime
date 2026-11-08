#include "host_service_registry.hpp"
#include "process_host.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace arm64metal;

int main() {
  assert(decoder_host_service_ids().at("_mach_vm_protect") == 196);
  assert(decoder_host_service_ids().at("_mach_vm_read_overwrite") == 197);
  assert(runner_host_service_ids().at("_mach_vm_protect") == 196);
  assert(runner_host_service_ids().at("_mach_vm_read_overwrite") == 197);

  ProcessBootstrapResult bootstrap;
  bootstrap.guestTaskPort = 77;
  HostServiceState state;
  bool protectCalled = false;
  state.machVmProtect = [&](std::uint64_t address, std::uint64_t size,
                            bool setMaximum, std::uint32_t protection) {
    protectCalled = address == 4096 && size == 8192 && setMaximum &&
                    protection == 5;
    return std::uint32_t{0};
  };
  GuestMemory protectMemory(std::vector<std::uint8_t>(256));
  std::vector<std::uint64_t> registers(36);
  registers[0] = bootstrap.guestTaskPort;
  registers[1] = 4096;
  registers[2] = 8192;
  registers[3] = 1;
  registers[4] = 5;
  (void)dispatch_guest_host_service(registers, protectMemory, bootstrap, state, 196);
  assert(registers[0] == 0 && protectCalled);
  registers[0] = 78;
  (void)dispatch_guest_host_service(registers, protectMemory, bootstrap, state, 196);
  assert(registers[0] == 0x10000003U);

  std::vector<std::uint8_t> initial(256, 0);
  std::memcpy(initial.data() + 32, "abcdefgh", 8);
  GuestMemory memory(std::move(initial));
  std::vector<std::uint64_t> readRegisters(36);
  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[1] = 32;
  readRegisters[2] = 6;
  readRegisters[3] = 34;
  readRegisters[4] = 200;
  std::uint64_t sentinel = UINT64_C(0x5555);
  std::memcpy(memory.buffer(200, sizeof(sentinel), true), &sentinel, sizeof(sentinel));
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 0);
  assert(std::memcmp(memory.buffer(32, 8), "ababcdef", 8) == 0);
  std::uint64_t copied = 0;
  std::memcpy(&copied, memory.buffer(200, sizeof(copied)), sizeof(copied));
  assert(copied == 6);

  std::memcpy(memory.buffer(32, 8, true), "ABCDEFGH", 8);
  std::memset(memory.buffer(100, 8, true), 0x5a, 8);
  sentinel = UINT64_C(0x7777);
  std::memcpy(memory.buffer(220, sizeof(sentinel), true), &sentinel, sizeof(sentinel));
  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[1] = 252;
  readRegisters[2] = 8;
  readRegisters[3] = 100;
  readRegisters[4] = 220;
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 1);
  assert(std::memcmp(memory.buffer(100, 8), "\x5a\x5a\x5a\x5a\x5a\x5a\x5a\x5a", 8) == 0);
  std::memcpy(&copied, memory.buffer(220, sizeof(copied)), sizeof(copied));
  assert(copied == UINT64_C(0x7777));

  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[1] = 32;
  readRegisters[2] = 8;
  readRegisters[3] = 252;
  readRegisters[4] = 220;
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 1);
  assert(std::memcmp(memory.buffer(252, 4), "ABCD", 4) == 0);
  std::memcpy(&copied, memory.buffer(220, sizeof(copied)), sizeof(copied));
  assert(copied == UINT64_C(0x7777));

  std::memset(memory.buffer(100, 8, true), 0x6b, 8);
  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[3] = 100;
  readRegisters[4] = 252;
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 1);
  assert(std::memcmp(memory.buffer(100, 8), "\x6b\x6b\x6b\x6b\x6b\x6b\x6b\x6b", 8) == 0);

  readRegisters[0] = bootstrap.guestTaskPort;
  readRegisters[1] = 32;
  readRegisters[2] = 0;
  readRegisters[3] = UINT64_C(0x1000);
  readRegisters[4] = 200;
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 0);
  std::memcpy(&copied, memory.buffer(200, sizeof(copied)), sizeof(copied));
  assert(copied == 0);

  readRegisters[0] = 78;
  readRegisters[1] = 32;
  readRegisters[2] = 8;
  readRegisters[3] = 100;
  readRegisters[4] = 200;
  (void)dispatch_guest_host_service(readRegisters, memory, bootstrap, state, 197);
  assert(readRegisters[0] == 0x10000003U);
  std::puts("named VM host service checks passed");
}
