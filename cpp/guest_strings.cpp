#include "guest_strings.hpp"

#include <sstream>
#include <utility>

namespace arm64metal {
namespace {

std::string mapped_address(
    bool cached,
    const GuestOffsetMapper& mapper,
    const std::string& address,
    bool write) {
    return cached ? mapper(address, "1ul", write) : address;
}

std::string byte_bounds(bool cached, const std::string& address, bool write) {
    if (cached) return address + " == ~0ul";
    return address + " >= size" +
           (write ? " || !guest_write(" + address + ", 1ul, writable)" : "");
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string make_body(
    std::string_view name,
    bool cached,
    const GuestOffsetMapper& mapper) {
    const bool strncpy = name == "_strncpy" || name == "___strncpy_chk";
    const bool strlcpy = name == "_strlcpy" || name == "___strlcpy_chk";
    const bool fortified = ends_with(name, "_chk");
    std::ostringstream out;
    out << "{\n";

    if (strncpy) {
        out << "ulong copied = 0;\n"
            << "for (; copied < x[2]; ++copied) {\n"
            << "if (x[1] > ~0ul - copied || x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }\n";
        const auto source = mapped_address(cached, mapper, "x[1] + copied", false);
        const auto destination = mapped_address(cached, mapper, "x[0] + copied", true);
        out << "ulong so = " << source << ";\n"
            << "ulong doff = " << destination << ";\n"
            << "if (" << byte_bounds(cached, cached ? "so" : "x[1] + copied", false) << " || "
            << byte_bounds(cached, cached ? "doff" : "x[0] + copied", true) << ") { x[33] = pc; x[34] = 2; return; }\n"
            << "uchar ch = memory[so]; memory[doff] = ch;\n"
            << "if (ch == 0) { ++copied; break; }\n}\n"
            << "for (; copied < x[2]; ++copied) {\n"
            << "if (x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }\n";
        const auto zero_destination = mapped_address(cached, mapper, "x[0] + copied", true);
        out << "ulong doff = " << zero_destination << ";\n"
            << "if (" << byte_bounds(cached, cached ? "doff" : "x[0] + copied", true)
            << ") { x[33] = pc; x[34] = 2; return; }\n"
            << "memory[doff] = 0;\n}\n";
    } else {
        out << "ulong sourceLength = 0;\n"
            << "for (;;) {\n"
            << "if (x[1] > ~0ul - sourceLength) { x[33] = pc; x[34] = 2; return; }\n";
        const auto source_at_end = mapped_address(cached, mapper, "x[1] + sourceLength", false);
        out << "ulong so = " << source_at_end << ";\n"
            << "if (" << byte_bounds(cached, cached ? "so" : "x[1] + sourceLength", false)
            << ") { x[33] = pc; x[34] = 2; return; }\n"
            << "if (memory[so] == 0) break;\n++sourceLength;\n}\n";

        if (strlcpy) {
            out << "ulong copied = min(sourceLength, x[2] == 0 ? 0ul : x[2] - 1ul);\n"
                << "for (ulong i = 0; i < copied; ++i) {\n"
                << "if (x[1] > ~0ul - i || x[0] > ~0ul - i) { x[33] = pc; x[34] = 2; return; }\n";
            const auto source = mapped_address(cached, mapper, "x[1] + i", false);
            const auto destination = mapped_address(cached, mapper, "x[0] + i", true);
            out << "ulong so = " << source << ";\n"
                << "ulong doff = " << destination << ";\n"
                << "if (" << byte_bounds(cached, cached ? "so" : "x[1] + i", false) << " || "
                << byte_bounds(cached, cached ? "doff" : "x[0] + i", true)
                << ") { x[33] = pc; x[34] = 2; return; }\n"
                << "memory[doff] = memory[so];\n}\n"
                << "if (x[2] != 0) {\n"
                << "if (x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }\n";
            const auto terminator = mapped_address(cached, mapper, "x[0] + copied", true);
            out << "ulong doff = " << terminator << ";\n"
                << "if (" << byte_bounds(cached, cached ? "doff" : "x[0] + copied", true)
                << ") { x[33] = pc; x[34] = 2; return; }\n"
                << "memory[doff] = 0;\n}\n"
                << "x[0] = sourceLength;\n";
        } else {
            out << "if (sourceLength == ~0ul || ("
                << (fortified ? "sourceLength + 1ul > x[2] || " : "")
                << "x[0] > ~0ul - sourceLength)) { x[33] = pc; x[34] = 2; return; }\n"
                << "for (ulong i = 0; i <= sourceLength; ++i) {\n"
                << "if (x[1] > ~0ul - i || x[0] > ~0ul - i) { x[33] = pc; x[34] = 2; return; }\n";
            const auto source = mapped_address(cached, mapper, "x[1] + i", false);
            const auto destination = mapped_address(cached, mapper, "x[0] + i", true);
            out << "ulong so = " << source << ";\n"
                << "ulong doff = " << destination << ";\n"
                << "if (" << byte_bounds(cached, cached ? "so" : "x[1] + i", false) << " || "
                << byte_bounds(cached, cached ? "doff" : "x[0] + i", true)
                << ") { x[33] = pc; x[34] = 2; return; }\n"
                << "memory[doff] = memory[so];\n}\n";
        }
    }

    out << "}\n";
    auto body = out.str();
    if (!body.empty() && body.back() == '\n') body.pop_back();
    std::ostringstream formatted;
    std::istringstream lines(body);
    std::string line;
    bool first = true;
    int depth = 0;
    while (std::getline(lines, line)) {
        if (!first) formatted << '\n';
        first = false;
        const bool closing_line = line == "}";
        if (closing_line) --depth;
        if (depth > 1) formatted << std::string(static_cast<std::size_t>(depth - 1) * 4, ' ');
        formatted << line;
        if (!closing_line) {
            for (const auto character : line) {
                if (character == '{') ++depth;
                if (character == '}') --depth;
            }
        }
    }
    return formatted.str();
}

std::string make_memchr_body(bool cached, const GuestOffsetMapper& mapper) {
    std::ostringstream out;
    out << "{\n"
        << "ulong inputAddress = x[0];\n"
        << "ulong matchAddress = 0;\n"
        << "bool found = false;\n"
        << "for (ulong i = 0; i < x[2]; ++i) {\n"
        << "if (inputAddress > ~0ul - i) { x[33] = pc; x[34] = 2; return; }\n";
    const auto source = mapped_address(cached, mapper, "inputAddress + i", false);
    out << "ulong so = " << source << ";\n"
        << "if (" << byte_bounds(cached, cached ? "so" : "inputAddress + i", false)
        << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "if (memory[so] == uchar(x[1])) { matchAddress = inputAddress + i; found = true; break; }\n"
        << "}\n"
        << "x[0] = found ? matchAddress : 0ul;\n"
        << "}\n";
    auto body = out.str();
    if (!body.empty() && body.back() == '\n') body.pop_back();
    return body;
}

std::string make_strchr_body(bool cached, const GuestOffsetMapper& mapper) {
    std::ostringstream out;
    out << "{\n"
        << "ulong inputAddress = x[0];\n"
        << "ulong resultAddress = 0;\n"
        << "uchar target = uchar(x[1]);\n"
        << "for (ulong i = 0; ; ++i) {\n"
        << "if (inputAddress > ~0ul - i) { x[33] = pc; x[34] = 2; return; }\n";
    const auto source = mapped_address(cached, mapper, "inputAddress + i", false);
    out << "ulong so = " << source << ";\n"
        << "if (" << byte_bounds(cached, cached ? "so" : "inputAddress + i", false)
        << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "uchar ch = memory[so];\n"
        << "if (ch == target) { resultAddress = inputAddress + i; break; }\n"
        << "if (ch == 0) break;\n"
        << "}\n"
        << "x[0] = resultAddress;\n"
        << "}";
    return out.str();
}

std::string make_strsep_body(bool cached, const GuestOffsetMapper& mapper) {
    std::ostringstream out;
    const auto readable_slot = cached ? mapper("x[0]", "8ul", false) : "x[0]";
    const auto writable_slot = cached ? mapper("x[0]", "8ul", true) : "x[0]";
    out << "{\n"
        << "if (";
    if (cached) {
        out << "(" << mapper("x[0]", "8ul", false) << ") == ~0ul";
    } else {
        out << "x[0] > size || 8ul > size - x[0]";
    }
    out << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "ulong slot = " << readable_slot << ";\n"
        << "ulong inputAddress = load_guest_word(memory + slot, 8);\n"
        << "if (inputAddress == 0) { x[0] = 0; } else {\n"
        << "uchar delimiterSet[256];\n"
        << "for (uint b = 0; b < 256; ++b) delimiterSet[b] = 0;\n"
        << "for (ulong j = 0; ; ++j) {\n"
        << "if (x[1] > ~0ul - j) { x[33] = pc; x[34] = 2; return; }\n";
    const auto delimiter = mapped_address(cached, mapper, "x[1] + j", false);
    out << "ulong delimiterOffset = " << delimiter << ";\n"
        << "if (" << byte_bounds(cached, cached ? "delimiterOffset" : "x[1] + j", false)
        << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "uchar delimiter = memory[delimiterOffset];\n"
        << "if (delimiter == 0) break;\n"
        << "delimiterSet[delimiter] = 1;\n"
        << "}\n"
        << "ulong nextAddress = 0;\n"
        << "ulong splitOffset = 0;\n"
        << "bool split = false;\n"
        << "for (ulong i = 0; ; ++i) {\n"
        << "if (inputAddress > ~0ul - i) { x[33] = pc; x[34] = 2; return; }\n";
    const auto input = mapped_address(cached, mapper, "inputAddress + i", false);
    out << "ulong inputOffset = " << input << ";\n"
        << "if (" << byte_bounds(cached, cached ? "inputOffset" : "inputAddress + i", false)
        << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "uchar ch = memory[inputOffset];\n"
        << "if (ch == 0) break;\n"
        << "if (delimiterSet[ch] != 0) {\n"
        << "if (inputAddress + i == ~0ul) { x[33] = pc; x[34] = 2; return; }\n";
    const auto writable_input = mapped_address(cached, mapper, "inputAddress + i", true);
    out << "ulong writableOffset = " << writable_input << ";\n"
        << "if (" << byte_bounds(cached, cached ? "writableOffset" : "inputAddress + i", true)
        << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "splitOffset = writableOffset;\n"
        << "nextAddress = inputAddress + i + 1ul;\n"
        << "split = true;\n"
        << "break;\n"
        << "}\n"
        << "}\n"
        << "if (";
    if (cached) {
        out << "(" << writable_slot << ") == ~0ul";
    } else {
        out << "!guest_write(x[0], 8ul, writable)";
    }
    out << ") { x[33] = pc; x[34] = 2; return; }\n"
        << "if (split) memory[splitOffset] = 0;\n"
        << "ulong writableSlot = " << writable_slot << ";\n"
        << "store_guest_word(memory + writableSlot, nextAddress, 8);\n"
        << "x[0] = inputAddress;\n"
        << "}\n"
        << "}\n";
    auto body = out.str();
    if (!body.empty() && body.back() == '\n') body.pop_back();
    return body;
}

}  // namespace

std::optional<GuestInstruction> guest_string_call(
    std::string_view name,
    std::optional<std::uint64_t> return_address,
    bool cached,
    const GuestOffsetMapper& map_offset,
    std::uint64_t guest_code_limit) {
    const bool bounded_copy = name == "_strncpy" || name == "___strncpy_chk" ||
                              name == "_strlcpy" || name == "___strlcpy_chk" ||
                              name == "_strcpy" || name == "___strcpy_chk";
    if (!bounded_copy && name != "_strsep" && name != "_memchr" && name != "_strchr") {
        return std::nullopt;
    }

    const bool fortified = ends_with(name, "_chk");
    const bool checked_strcpy = name == "___strcpy_chk";
    GuestInstruction result;
    if (return_address) result.code.push_back("x[30] = " + std::to_string(*return_address) + "ul;");
    if (fortified && !checked_strcpy) {
        result.code.push_back("if (x[2] > x[3]) { x[33] = pc; x[34] = 2; return; }");
    }
    if (name == "_strsep") {
        result.code.push_back(make_strsep_body(cached, map_offset));
    } else if (name == "_memchr") {
        result.code.push_back(make_memchr_body(cached, map_offset));
    } else if (name == "_strchr") {
        result.code.push_back(make_strchr_body(cached, map_offset));
    } else {
        result.code.push_back(make_body(name, cached, map_offset));
    }
    result.code.push_back("if (x[30] == ~0ul) { x[34] = 0; return; }");
    result.code.push_back("if (x[30] >= " + std::to_string(guest_code_limit) +
                          "ul) { x[34] = 3; return; }");
    result.code.push_back("pc = x[30]; break;");
    if (return_address) result.next.push_back(*return_address);
    return result;
}

}  // namespace arm64metal
