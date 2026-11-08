#include "function_runner.hpp"

#include "macho.hpp"
#include "metal_backend.hpp"
#include "translate_core.hpp"
#include "integer_ops_shared.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace arm64metal::function_runner {
namespace {

struct Invocation {
  std::vector<std::uint32_t> words;
  std::uint32_t seed = 0;
};

struct Execution {
  std::vector<std::uint32_t> words;
  std::uint32_t result = 0;
};

class JsonInvocationParser {
 public:
  explicit JsonInvocationParser(const std::string& input) : input_(input) {}

  std::vector<Invocation> parse() {
    skip_space();
    expect('[');
    std::vector<Invocation> invocations;
    skip_space();
    if (consume(']')) {
      finish();
      return invocations;
    }
    while (true) {
      invocations.push_back(parse_invocation());
      skip_space();
      if (consume(']')) break;
      expect(',');
    }
    finish();
    return invocations;
  }

 private:
  const std::string& input_;
  std::size_t position_ = 0;
  std::size_t depth_ = 0;

  [[noreturn]] void invalid() const { throw std::runtime_error("Invalid invocation JSON"); }

  void skip_space() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  bool consume(char expected) {
    skip_space();
    if (position_ == input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }

  void expect(char expected) {
    if (!consume(expected)) invalid();
  }

  static void append_utf8(std::string& value, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
      value.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      value.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
      value.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      value.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
      value.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }

  std::uint32_t hex_quad() {
    if (input_.size() - position_ < 4) invalid();
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char digit = input_[position_++];
      result <<= 4;
      if (digit >= '0' && digit <= '9') result |= static_cast<std::uint32_t>(digit - '0');
      else if (digit >= 'a' && digit <= 'f') result |= static_cast<std::uint32_t>(digit - 'a' + 10);
      else if (digit >= 'A' && digit <= 'F') result |= static_cast<std::uint32_t>(digit - 'A' + 10);
      else invalid();
    }
    return result;
  }

  std::string parse_string() {
    expect('"');
    std::string result;
    while (position_ < input_.size()) {
      const auto ch = static_cast<unsigned char>(input_[position_++]);
      if (ch == '"') return result;
      if (ch < 0x20) invalid();
      if (ch != '\\') {
        result.push_back(static_cast<char>(ch));
        continue;
      }
      if (position_ == input_.size()) invalid();
      const char escape = input_[position_++];
      switch (escape) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          auto codepoint = hex_quad();
          if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') invalid();
            position_ += 2;
            const auto low = hex_quad();
            if (low < 0xdc00 || low > 0xdfff) invalid();
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
          } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            invalid();
          }
          append_utf8(result, codepoint);
          break;
        }
        default: invalid();
      }
    }
    invalid();
  }

  std::uint32_t parse_uint32() {
    skip_space();
    const auto start = position_;
    if (position_ == input_.size() || input_[position_] < '0' || input_[position_] > '9') invalid();
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) invalid();
    } else {
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    std::uint64_t value = 0;
    for (auto i = start; i < position_; ++i) {
      value = value * 10 + static_cast<unsigned>(input_[i] - '0');
      if (value > std::numeric_limits<std::uint32_t>::max()) invalid();
    }
    skip_space();
    if (position_ < input_.size() &&
        (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) invalid();
    return static_cast<std::uint32_t>(value);
  }

  std::vector<std::uint32_t> parse_words() {
    expect('[');
    std::vector<std::uint32_t> words;
    if (consume(']')) return words;
    while (true) {
      words.push_back(parse_uint32());
      if (consume(']')) break;
      expect(',');
    }
    return words;
  }

  Invocation parse_invocation() {
    if (++depth_ > 256) invalid();
    struct DepthGuard {
      std::size_t& depth;
      ~DepthGuard() { --depth; }
    } guard{depth_};

    expect('{');
    Invocation invocation;
    bool has_words = false;
    bool has_seed = false;
    if (!consume('}')) {
      while (true) {
        const auto key = parse_string();
        expect(':');
        if (key == "words") {
          invocation.words = parse_words();
          has_words = true;
        } else if (key == "seed") {
          invocation.seed = parse_uint32();
          has_seed = true;
        } else {
          skip_value();
        }
        if (consume('}')) break;
        expect(',');
      }
    }
    if (!has_words || !has_seed) invalid();
    return invocation;
  }

  void skip_number() {
    const auto start = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ == input_.size()) invalid();
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) invalid();
    } else {
      if (input_[position_] < '1' || input_[position_] > '9') invalid();
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const auto digits = position_;
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
      if (digits == position_) invalid();
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
      const auto digits = position_;
      while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
      if (digits == position_) invalid();
    }
    if (start == position_) invalid();
  }

  void skip_literal(const char* literal) {
    const std::string expected(literal);
    if (input_.compare(position_, expected.size(), expected) != 0) invalid();
    position_ += expected.size();
  }

  void skip_value() {
    skip_space();
    if (position_ == input_.size() || ++depth_ > 256) invalid();
    struct DepthGuard {
      std::size_t& depth;
      ~DepthGuard() { --depth; }
    } guard{depth_};
    switch (input_[position_]) {
      case '"': (void)parse_string(); return;
      case '{':
        ++position_;
        if (consume('}')) return;
        while (true) {
          (void)parse_string();
          expect(':');
          skip_value();
          if (consume('}')) return;
          expect(',');
        }
      case '[':
        ++position_;
        if (consume(']')) return;
        while (true) {
          skip_value();
          if (consume(']')) return;
          expect(',');
        }
      case 't': skip_literal("true"); return;
      case 'f': skip_literal("false"); return;
      case 'n': skip_literal("null"); return;
      default: skip_number(); return;
    }
  }

  void finish() {
    skip_space();
    if (position_ != input_.size()) invalid();
  }
};

std::string make_kernel(const MachO& image, const std::string& symbol) {
  const auto region = image.symbol(symbol);
  std::map<std::int64_t, translate_core::Instruction> instructions;
  std::vector<std::int64_t> pending{static_cast<std::int64_t>(region.entry)};
  while (!pending.empty()) {
    const auto pc = pending.back();
    pending.pop_back();
    if (instructions.find(pc) != instructions.end()) continue;
    if (region.limit < 4 || pc < static_cast<std::int64_t>(region.entry) ||
        pc > static_cast<std::int64_t>(region.limit - 4) || pc % 4 != 0) {
      throw std::runtime_error("Control flow leaves supported function extent");
    }
    const auto instruction = translate_core::decode(
        image.word(region.text_offset + static_cast<std::size_t>(pc)), pc);
    instructions.emplace(pc, instruction);
    pending.insert(pending.end(), instruction.next.begin(), instruction.next.end());
  }
  const auto cases = translate_core::render_blocks(
      instructions, static_cast<std::int64_t>(region.entry));
  std::ostringstream source;
  source << "#include <metal_stdlib>\n"
            "using namespace metal;\n"
         << typed_integer_msl_source
         << "\n"
         <<
            "kernel void run_guest(device uint *memory [[buffer(0)]],\n"
            "                      device const uint *counts [[buffer(1)]],\n"
            "                      device const uint *seeds [[buffer(2)]],\n"
            "                      device uint *returns [[buffer(3)]],\n"
            "                      device uint *status [[buffer(4)]],\n"
            "                      constant uint &stride [[buffer(5)]],\n"
            "                      constant uint &max_blocks [[buffer(6)]],\n"
            "                      uint id [[thread_position_in_grid]]) {\n"
            "    device uint *words = memory + ulong(id) * stride;\n"
            "    uint count = counts[id];\n"
            "    if (count > stride) { status[id] = 2; return; }\n"
            "    ulong x[31] = {};\n"
            "    x[1] = count;\n"
            "    x[2] = seeds[id];\n"
            "    bool z = false;\n"
            "    uint pc = " << region.entry << "u;\n"
            "    for (uint step = 0; step < max_blocks; ++step) {\n"
            "        switch (pc) {\n";
  std::istringstream cases_in(cases);
  std::string line;
  while (std::getline(cases_in, line)) source << line << '\n';
  source << "        default: status[id] = 3; return;\n"
             "        }\n"
             "    }\n"
             "    status[id] = 1;\n"
             "}\n";
  return source.str();
}

std::size_t checked_product(std::size_t left, std::size_t right, const char* message) {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::runtime_error(message);
  }
  return left * right;
}

template <typename T>
SharedBuffer make_buffer(const Backend& backend, const std::vector<T>& values) {
  const auto bytes = checked_product(values.size(), sizeof(T), "Metal buffer size overflow");
  auto buffer = backend.makeSharedBuffer(bytes);
  if (!values.empty()) std::memcpy(buffer.data(), values.data(), bytes);
  return buffer;
}

class MetalGuest {
 public:
  explicit MetalGuest(const std::string& source)
      : backend_(Backend::createDefault()), kernel_(backend_.compileCompute(source, "run_guest")) {}

  std::vector<Execution> run(const std::vector<Invocation>& inputs, std::uint32_t budget) const {
    if (inputs.empty()) return {};
    std::size_t stride = 1;
    for (const auto& input : inputs) stride = std::max(stride, input.words.size());
    if (stride > 65'536 || inputs.size() > 65'536 ||
        checked_product(stride, inputs.size(), "Guest batch exceeds the runtime's buffer limits") > 16'777'216) {
      throw std::runtime_error("Guest batch exceeds the runtime's buffer limits");
    }
    std::vector<std::uint32_t> words(stride * inputs.size(), 0);
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> seeds;
    counts.reserve(inputs.size());
    seeds.reserve(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const auto offset = index * stride;
      std::copy(inputs[index].words.begin(), inputs[index].words.end(), words.begin() + offset);
      counts.push_back(static_cast<std::uint32_t>(inputs[index].words.size()));
      seeds.push_back(inputs[index].seed);
    }
    std::vector<std::uint32_t> results(inputs.size(), 0);
    std::vector<std::uint32_t> statuses(inputs.size(), std::numeric_limits<std::uint32_t>::max());
    auto memory_buffer = make_buffer(backend_, words);
    auto count_buffer = make_buffer(backend_, counts);
    auto seed_buffer = make_buffer(backend_, seeds);
    auto output_buffer = make_buffer(backend_, results);
    auto status_buffer = make_buffer(backend_, statuses);
    auto stride_buffer = make_buffer(backend_, std::vector<std::uint32_t>{static_cast<std::uint32_t>(stride)});
    auto budget_buffer = make_buffer(backend_, std::vector<std::uint32_t>{budget});
    const std::vector<BufferBinding> bindings = {
        {&memory_buffer, 0, 0}, {&count_buffer, 1, 0}, {&seed_buffer, 2, 0},
        {&output_buffer, 3, 0}, {&status_buffer, 4, 0},
        {&stride_buffer, 5, 0}, {&budget_buffer, 6, 0}};
    (void)backend_.dispatch(kernel_, bindings, DispatchSize{inputs.size(), 1, 1},
                            DispatchSize{1, 1, 1});
    std::memcpy(words.data(), memory_buffer.data(), words.size() * sizeof(std::uint32_t));
    std::memcpy(results.data(), output_buffer.data(), results.size() * sizeof(std::uint32_t));
    std::memcpy(statuses.data(), status_buffer.data(), statuses.size() * sizeof(std::uint32_t));

    std::vector<Execution> executions;
    executions.reserve(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      if (statuses[index] != 0) {
        const std::map<std::uint32_t, std::string> reasons = {
            {1, "block budget exhausted"}, {2, "guest memory out of bounds"},
            {3, "invalid guest PC"}};
        const auto found = reasons.find(statuses[index]);
        const auto reason = found == reasons.end() ? "unknown guest status" : found->second;
        throw std::runtime_error("Invocation " + std::to_string(index) + ": " + reason);
      }
      const auto offset = index * stride;
      const auto first_padding = words.begin() + offset + inputs[index].words.size();
      const auto last_padding = words.begin() + offset + stride;
      if (!std::all_of(first_padding, last_padding, [](std::uint32_t value) { return value == 0; })) {
        throw std::runtime_error("Invocation " + std::to_string(index) + ": modified padding");
      }
      Execution execution;
      execution.words.assign(words.begin() + offset,
                             words.begin() + offset + inputs[index].words.size());
      execution.result = results[index];
      executions.push_back(std::move(execution));
    }
    return executions;
  }

  const char* device_name() const noexcept { return backend_.deviceName(); }

 private:
  Backend backend_;
  ComputeKernel kernel_;
};

std::string serialize(const std::vector<Execution>& results) {
  std::ostringstream json;
  json << '[';
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index != 0) json << ',';
    json << "{\"result\":" << results[index].result << ",\"words\":[";
    for (std::size_t word = 0; word < results[index].words.size(); ++word) {
      if (word != 0) json << ',';
      json << results[index].words[word];
    }
    json << "]}";
  }
  json << ']';
  return json.str();
}

void close_library(void* handle) noexcept {
  if (handle != nullptr) dlclose(handle);
}

}  // namespace

std::string translate(const std::filesystem::path& binary, const std::string& symbol) {
  return make_kernel(MachO(binary), symbol);
}

std::string run(const std::filesystem::path& binary, const std::string& symbol,
                const std::string& json_input, std::uint32_t budget) {
  if (budget == 0 || budget > 1'000'000) throw std::runtime_error("Invalid block budget");
  const auto inputs = JsonInvocationParser(json_input).parse();
  MetalGuest guest(translate(binary, symbol));
  return serialize(guest.run(inputs, budget));
}

void verify(const std::filesystem::path& binary, const std::string& symbol) {
  void* handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const auto* error = dlerror();
    throw std::runtime_error(error == nullptr ? "Unable to load trusted native dylib" : error);
  }
  const auto close = std::unique_ptr<void, decltype(&close_library)>(handle, close_library);
  (void)close;
  dlerror();
  void* address = dlsym(handle, symbol.c_str());
  if (const auto* error = dlerror()) throw std::runtime_error(error);
  if (address == nullptr) throw std::runtime_error("Native symbol unavailable: " + symbol);
  using Native = std::uint32_t (*)(std::uint32_t*, std::uint64_t, std::uint32_t);
  Native native = nullptr;
  static_assert(sizeof(native) == sizeof(address), "Function and data pointers must have equal size");
  std::memcpy(&native, &address, sizeof(native));

  std::uint32_t state = 0x136cf03d;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  std::vector<Invocation> inputs;
  inputs.reserve(1024);
  for (std::size_t index = 0; index < 1024; ++index) {
    Invocation invocation;
    const auto count = index % 65;
    invocation.words.reserve(count);
    for (std::size_t word = 0; word < count; ++word) invocation.words.push_back(next());
    invocation.seed = next();
    inputs.push_back(std::move(invocation));
  }

  MetalGuest guest(translate(binary, symbol));
  const auto actual = guest.run(inputs, 66);
  std::vector<Execution> expected;
  expected.reserve(inputs.size());
  for (const auto& input : inputs) {
    auto words = input.words;
    const auto result = native(words.empty() ? nullptr : words.data(), words.size(), input.seed);
    expected.push_back(Execution{std::move(words), result});
  }
  if (actual.size() != expected.size()) throw std::runtime_error("GPU output differs from native output");
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (actual[index].result != expected[index].result || actual[index].words != expected[index].words) {
      throw std::runtime_error("GPU output differs from native output: " + std::to_string(index));
    }
  }
  try {
    (void)guest.run({inputs.front()}, 0);
    throw std::runtime_error("Guest accepted a zero block budget");
  } catch (const std::runtime_error& error) {
    if (std::string(error.what()).find("block budget exhausted") == std::string::npos) throw;
  }
  std::cout << symbol << " on " << guest.device_name()
            << ": " << actual.size() << " exact native/GPU matches; block budget enforced\n";
}

}  // namespace arm64metal::function_runner
