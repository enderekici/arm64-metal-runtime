#include "guest_format.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <string_view>

namespace arm64metal {
namespace {

constexpr std::size_t kMaximumFieldWidth = 4096;

void append_text(std::vector<std::uint8_t>& output, std::string_view text) {
    output.insert(output.end(), text.begin(), text.end());
}

void append_padded(std::vector<std::uint8_t>& output, std::string_view text,
                   std::size_t width, bool zero_pad, bool alternate,
                   char conversion) {
    std::string prefix;
    if (alternate && (conversion == 'x' || conversion == 'X') && text != "0") {
        prefix = conversion == 'X' ? "0X" : "0x";
    } else if (alternate && conversion == 'o' &&
               (text.empty() || text.front() != '0')) {
        prefix = "0";
    }

    const auto length = prefix.size() + text.size();
    if (length >= width) {
        append_text(output, prefix);
        append_text(output, text);
        return;
    }
    const auto padding = width - length;
    if (!zero_pad) {
        output.insert(output.end(), padding, static_cast<std::uint8_t>(' '));
        append_text(output, prefix);
        append_text(output, text);
        return;
    }
    append_text(output, prefix);
    if (!text.empty() && text.front() == '-') {
        output.push_back('-');
        output.insert(output.end(), padding, static_cast<std::uint8_t>('0'));
        append_text(output, std::string_view(text).substr(1));
        return;
    }
    output.insert(output.end(), padding, static_cast<std::uint8_t>('0'));
    append_text(output, text);
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

        bool alternate = false;
        bool zero_pad = false;
        while (index < format.size() && (format[index] == '#' || format[index] == '0')) {
            if (format[index] == '#') alternate = true;
            if (format[index] == '0') zero_pad = true;
            ++index;
        }
        std::size_t width = 0;
        while (index < format.size() && format[index] >= '0' && format[index] <= '9') {
            const auto digit = static_cast<std::size_t>(format[index++] - '0');
            if (width > (kMaximumFieldWidth - digit) / 10) {
                throw GuestFormatError("Guest printf field width exceeds limit");
            }
            width = width * 10 + digit;
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
        if (conversion == '%' && !wide && width == 0 && !alternate && !zero_pad) {
            output.push_back('%');
            continue;
        }
        const bool supported = conversion == 'd' || conversion == 'i' || conversion == 'u' ||
                               conversion == 'x' || conversion == 'X' || conversion == 'o' ||
                               conversion == 'c' || conversion == 's';
        if (!supported || (wide && (conversion == 'c' || conversion == 's'))) {
            throw GuestFormatError("Unsupported guest printf conversion");
        }
        if ((conversion == 'c' || conversion == 's') && alternate) {
            throw GuestFormatError("Unsupported guest printf conversion flags");
        }

        const auto raw = argument();
        const auto number = wide ? raw : static_cast<std::uint32_t>(raw);
        switch (conversion) {
            case 'd':
            case 'i':
                if (wide) {
                    append_padded(output,
                                  integer_text(bit_cast_signed<std::int64_t>(raw), 10),
                                  width, zero_pad, alternate, conversion);
                } else {
                    append_padded(output,
                                  integer_text(bit_cast_signed<std::int32_t>(
                                      static_cast<std::uint32_t>(number)), 10),
                                  width, zero_pad, alternate, conversion);
                }
                break;
            case 'u':
                append_padded(output, integer_text(number, 10), width, zero_pad,
                              alternate, conversion);
                break;
            case 'x':
                append_padded(output, integer_text(number, 16), width, zero_pad,
                              alternate, conversion);
                break;
            case 'X':
                append_padded(output, integer_text(number, 16, true), width,
                              zero_pad, alternate, conversion);
                break;
            case 'o':
                append_padded(output, integer_text(number, 8), width, zero_pad,
                              alternate, conversion);
                break;
            case 'c': {
                const auto character = static_cast<std::uint8_t>(raw);
                if (width > 1) output.insert(output.end(), width - 1, ' ');
                output.push_back(character);
                break;
            }
            case 's': {
                const auto bytes = string(raw);
                if (bytes.size() < width) output.insert(output.end(), width - bytes.size(), ' ');
                output.insert(output.end(), bytes.begin(), bytes.end());
                break;
            }
            default: throw GuestFormatError("Unsupported guest printf conversion");
        }
    }
    return output;
}

}  // namespace arm64metal
