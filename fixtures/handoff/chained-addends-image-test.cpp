#include "image_loader.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  assert(argc == 3);
  const arm64metal::ExecutableImage image(argv[2], true, false);
  const std::string mode(argv[1]);
  if (mode == "strong") {
    std::vector<std::int64_t> targets;
    for (const auto& [address, name] : image.bindings) {
      if (name.find("_chained_addend_target") == std::string::npos) continue;
      const auto target = image.function_bindings.find(address);
      assert(target != image.function_bindings.end());
      targets.push_back(target->second);
    }
    std::sort(targets.begin(), targets.end());
    assert(targets.size() >= 2);
    assert(targets[1] - targets[0] == 12);
    std::cout << "strong_addend=12\n";
  } else {
    assert(mode == "weak");
    bool foundWeakNullAddend = false;
    for (const auto& [address, name] : image.bindings) {
      if (name.find("_chained_missing_target") == std::string::npos) continue;
      const auto target = image.function_bindings.find(address);
      assert(target != image.function_bindings.end());
      assert(target->second == 12);
      foundWeakNullAddend = true;
    }
    assert(foundWeakNullAddend);
    std::cout << "weak_null_addend=12\n";
  }
}
