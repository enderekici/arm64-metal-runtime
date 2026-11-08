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
  using namespace arm64metal;
  assert(decoder_host_service_ids().at("___memset_chk") ==
         kMemsetChkFailureService);
  assert(runner_host_service_ids().at("___memset_chk") ==
         kMemsetChkFailureService);

  const auto direct = hostCall("___memset_chk", 0x1004, false);
  assert(has_code(direct, "if (x[2] > x[3])"));
  assert(has_code(direct, "x[35] = " +
      std::to_string(kMemsetChkFailureService) + "ul"));
  assert(has_code(direct, "guest_fill(memory + x[0], uchar(x[1]), x[2])"));
  assert(has_code(direct, "x[0] > size || x[2] > size - x[0]"));
  const auto direct_size_check = std::find_if(direct.code.begin(), direct.code.end(),
      [](const auto& line) { return line.find("if (x[2] > x[3])") != std::string::npos; });
  const auto direct_fill = std::find_if(direct.code.begin(), direct.code.end(),
      [](const auto& line) { return line.find("guest_fill(") != std::string::npos; });
  assert(direct_size_check < direct_fill);

  const auto cached = hostCall("___memset_chk", 0x2004, true);
  assert(has_code(cached, "if (x[2] > x[3])"));
  assert(has_code(cached, "guest_offset(x[0], x[2], size, writable, true"));
  assert(has_code(cached, "guest_fill(memory + guest_offset"));

  const auto memset = hostCall("_memset", 0x3004, false);
  assert(has_code(memset, "guest_fill("));
  assert(!has_code(memset, "if (x[2] > x[3])"));
  return 0;
}
