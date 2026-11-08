#include "guest_memory_msl.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) throw std::invalid_argument("expected input and output paths");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("unable to read interpreter template");
    std::string source{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
    constexpr std::string_view marker = "@GUEST_MEMORY_MSL_SOURCE@";
    std::size_t position = 0;
    while ((position = source.find(marker, position)) != std::string::npos) {
        source.replace(position, marker.size(), arm64metal::guestMemoryMslSource);
        position += arm64metal::guestMemoryMslSource.size();
    }
    if (source.find(marker) != std::string::npos) {
        throw std::runtime_error("interpreter template contains unresolved helper marker");
    }
    std::ofstream output(argv[2], std::ios::binary);
    if (!output) throw std::runtime_error("unable to write interpreter source");
    output << source;
    if (!output) throw std::runtime_error("failed while writing interpreter source");
    return 0;
}
