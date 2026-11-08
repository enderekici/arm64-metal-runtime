#include "host_service_registry.hpp"
#include "process_decoder.hpp"

#include <algorithm>
#include <cassert>
#include <string>

namespace {

bool has_code(const arm64metal::Instruction& instruction, const std::string& needle) {
  return std::any_of(instruction.code.begin(), instruction.code.end(),
                     [&](const auto& line) { return line.find(needle) != std::string::npos; });
}

}  // namespace

int main() {
  assert(arm64metal::decoder_host_service_ids().at("___memcpy_chk") ==
         arm64metal::kMemcpyChkFailureService);
  assert(arm64metal::runner_host_service_ids().at("___memcpy_chk") ==
         arm64metal::kMemcpyChkFailureService);

  const auto direct = arm64metal::hostCall("___memcpy_chk", 0x1004, false);
  assert(has_code(direct, "if (x[2] > x[3])"));
  assert(has_code(direct, "x[35] = " +
      std::to_string(arm64metal::kMemcpyChkFailureService) + "ul"));
  assert(has_code(direct, "guest_copy(memory + x[0], memory + x[1], x[2])"));
  assert(has_code(direct, "x[0] > size || x[2] > size - x[0]"));
  assert(has_code(direct, "x[1] > size || x[2] > size - x[1]"));
  const auto direct_size_check = std::find_if(direct.code.begin(), direct.code.end(),
      [](const auto& line) { return line.find("if (x[2] > x[3])") != std::string::npos; });
  const auto direct_copy = std::find_if(direct.code.begin(), direct.code.end(),
      [](const auto& line) { return line.find("guest_copy(") != std::string::npos; });
  assert(direct_size_check < direct_copy);

  const auto cached = arm64metal::hostCall("___memcpy_chk", 0x2004, true);
  assert(has_code(cached, "if (x[2] > x[3])"));
  assert(has_code(cached, "guest_offset(x[0], x[2], size, writable, true"));
  assert(has_code(cached, "guest_offset(x[1], x[2], size, writable, false"));
  assert(has_code(cached, "guest_copy(memory + guest_offset"));

  const auto memcpy = arm64metal::hostCall("_memcpy", 0x3004, false);
  const auto memmove = arm64metal::hostCall("_memmove", 0x4004, false);
  assert(has_code(memcpy, "guest_copy("));
  assert(!has_code(memcpy, "guest_move("));
  assert(has_code(memmove, "guest_move("));
  return 0;
}
