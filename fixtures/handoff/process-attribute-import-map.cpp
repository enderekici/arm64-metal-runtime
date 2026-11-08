#include "process_decoder.hpp"

#include <array>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

int main() {
    constexpr std::array<std::pair<std::string_view, int>, 10> services{{
        {"_pthread_attr_init", 203},
        {"_pthread_attr_destroy", 204},
        {"_pthread_attr_setstacksize", 205},
        {"_pthread_attr_setguardsize", 206},
        {"_pthread_attr_setdetachstate", 207},
        {"_pthread_attr_getstacksize", 208},
        {"_pthread_attr_getguardsize", 209},
        {"_pthread_attr_getdetachstate", 210},
        {"_pthread_attr_setstack", 211},
        {"_pthread_attr_getstack", 212},
    }};
    for (const auto& [name, service] : services) {
        const auto instruction = arm64metal::hostCall(std::string(name), 0x1000, true);
        const auto expected = "x[35] = " + std::to_string(service) + "ul; x[34] = 4; return;";
        if (instruction.code.size() < 4 || instruction.code[3] != expected ||
            instruction.next != std::vector<std::int64_t>{0x1000}) {
            std::cerr << "bad process host-service mapping: " << name << '\n';
            return 1;
        }
    }
    std::cout << "pthread attribute imports map to services 203-212\n";
    return 0;
}
