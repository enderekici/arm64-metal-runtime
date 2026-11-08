#include "guest_code_policy.hpp"
#include "guest_state.hpp"
#include "macho.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  arm64metal::GuestThread first_thread;
  arm64metal::GuestThread second_thread;
  require(first_thread.jitWriteProtect && second_thread.jitWriteProtect,
          "guest threads do not start in write-protected mode");
  second_thread.jitWriteProtect = false;
  require(first_thread.jitWriteProtect,
          "JIT write-protection state is not isolated per guest thread");
  const arm64metal::MachO without_jit(argv[1], true);
  const arm64metal::MachO with_jit(argv[2], true);
  const auto denied = arm64metal::guest_code_policy(argv[1], without_jit.bytes());
  require(denied.validation == arm64metal::GuestCodeValidation::valid,
          "hardened ad-hoc signature without allow-jit did not validate");
  require(denied.hardened_runtime && denied.ad_hoc && !denied.allow_jit &&
              !denied.map_jit_eligible,
          "hardened ad-hoc without allow-jit was not denied");

  const auto allowed = arm64metal::guest_code_policy(argv[2], with_jit.bytes());
  require(allowed.validation == arm64metal::GuestCodeValidation::valid,
          "hardened ad-hoc allow-jit signature did not validate");
  require(allowed.hardened_runtime && allowed.ad_hoc && allowed.allow_jit &&
              allowed.map_jit_eligible,
          "hardened ad-hoc allow-jit was not eligible");

  auto tampered = with_jit.bytes();
  tampered.at(4096) ^= 1;
  const auto rejected = arm64metal::guest_code_policy(argv[2], tampered);
  require(rejected.validation == arm64metal::GuestCodeValidation::invalid &&
              !rejected.map_jit_eligible &&
              rejected.image_sha256 != allowed.image_sha256,
          "tampered executable bytes were not rejected");
  std::cout << "hardened_no_jit=denied hardened_allow_jit=eligible tampered=invalid\n";
}
