#include "guest_format.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <string_view>

namespace arm64metal {
namespace {

void append_text(std::vector<std::uint8_t>& output, std::string_view text) {
    output.insert(output.end(), text.begin(), text.end());
}

template <typename Integer>
std::string integer_text(Integer value, int base, bool uppercase = false) {
    std::array<char, 66> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
    if (converted.ec != std::errc{}) throw GuestFormatError("Unable to format guest integer");
    std::string result(buffer.data(), converted.ptr);
    if (uppercase) {
        for (auto& character : result) {
            if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        }
    }
    return result;
}

template <typename Signed, typename Unsigned>
Signed bit_cast_signed(Unsigned value) {
    static_assert(sizeof(Signed) == sizeof(Unsigned));
    Signed result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

}  // namespace

std::vector<std::uint8_t> format_guest_output(
    const std::vector<std::uint8_t>& format,
    const GuestArgumentReader& argument,
    const GuestStringReader& string) {
    std::vector<std::uint8_t> output;
    std::size_t index = 0;
    while (index < format.size()) {
        const auto byte = format[index++];
        if (byte != '%') {
            output.push_back(byte);
            continue;
        }

        bool wide = false;
        if (index < format.size() && (format[index] == 'j' || format[index] == 'l' ||
                                      format[index] == 't' || format[index] == 'z')) {
            wide = true;
            const auto modifier = format[index++];
            if (modifier == 'l' && index < format.size() && format[index] == 'l') ++index;
        }
        if (index >= format.size()) throw GuestFormatError("Incomplete guest printf conversion");

        const auto conversion = format[index++];
        if (conversion == '%' && !wide) {
            output.push_back('%');
            continue;
        }
        const bool supported = conversion == 'd' || conversion == 'i' || conversion == 'u' ||
                               conversion == 'x' || conversion == 'X' || conversion == 'o' ||
                               conversion == 'c' || conversion == 's';
        if (!supported || (wide && (conversion == 'c' || conversion == 's'))) {
            throw GuestFormatError("Unsupported guest printf conversion");
        }

        const auto raw = argument();
        const auto number = wide ? raw : static_cast<std::uint32_t>(raw);
        switch (conversion) {
            case 'd':
            case 'i':
                if (wide) {
                    append_text(output, integer_text(bit_cast_signed<std::int64_t>(raw), 10));
                } else {
                    append_text(output, integer_text(bit_cast_signed<std::int32_t>(
                        static_cast<std::uint32_t>(number)), 10));
                }
                break;
            case 'u': append_text(output, integer_text(number, 10)); break;
            case 'x': append_text(output, integer_text(number, 16)); break;
            case 'X': append_text(output, integer_text(number, 16, true)); break;
            case 'o': append_text(output, integer_text(number, 8)); break;
            case 'c': output.push_back(static_cast<std::uint8_t>(raw)); break;
            case 's': {
                const auto bytes = string(raw);
                output.insert(output.end(), bytes.begin(), bytes.end());
                break;
            }
            default: throw GuestFormatError("Unsupported guest printf conversion");
        }
    }
    return output;
}

}  // namespace arm64metal
