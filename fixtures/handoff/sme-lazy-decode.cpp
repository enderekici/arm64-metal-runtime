#include "process_sme_lazy.hpp"

#include <cassert>
#include <string>

int main() {
  using arm64metal::decode_sme_lazy;

  const auto addNegative = decode_sme_lazy(0x042f5feeu, 0x1000, true);
  assert(addNegative);
  assert(addNegative->code.size() == 1);
  assert(addNegative->code[0] == "x[14] = x[15] + (-1l * x[883]);");

  const auto addStackPointer = decode_sme_lazy(0x0430583fu, 0x1004, true);
  assert(addStackPointer);
  assert(addStackPointer->code.size() == 1);
  assert(addStackPointer->code[0] == "x[31] = x[16] + (1l * x[883]);");

  const auto readNegative = decode_sme_lazy(0x04bf5ff1u, 0x1008, true);
  assert(readNegative);
  assert(readNegative->code.size() == 1);
  assert(readNegative->code[0] == "x[17] = ulong(-1l * x[883]);");

  const auto registerBase = decode_sme_lazy(0xe1000000u, 0x100c, true);
  assert(registerBase);
  assert(registerBase->code.size() == 1);
  assert(registerBase->code[0].find("& 15ul") == std::string::npos);

  const auto stackBase = decode_sme_lazy(0xe10003e0u, 0x1010, true);
  assert(stackBase);
  assert(stackBase->code.size() == 1);
  assert(stackBase->code[0].find("x[31] & 15ul") != std::string::npos);
}
