#ifndef ARM64METAL_FUNCTION_RUNNER_HPP
#define ARM64METAL_FUNCTION_RUNNER_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace arm64metal::function_runner {

// Translate a named ARM64 function in a Mach-O image into the standalone Metal
// kernel used by the original `translate` command.
std::string translate(const std::filesystem::path& binary, const std::string& symbol);

// Run a JSON array of {"words":[...],"seed":...} invocations and return the
// sorted-key JSON result array. The input has the same limits and guest status
// checks as the original batch runner.
std::string run(const std::filesystem::path& binary, const std::string& symbol,
                const std::string& json_input, std::uint32_t budget = 65'536);

// Compare 1,024 deterministic native invocations with Metal and verify the
// zero block-budget rejection path. Throws std::runtime_error on failure.
void verify(const std::filesystem::path& binary, const std::string& symbol);

}  // namespace arm64metal::function_runner

#endif  // ARM64METAL_FUNCTION_RUNNER_HPP
