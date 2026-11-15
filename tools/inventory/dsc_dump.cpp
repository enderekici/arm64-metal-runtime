// Dump __text of dyld shared cache images plus their exports (guest-space addresses).
// usage: dsc-dump OUTDIR IMAGE_PATH...   (exact install names)
// Also `str` lines: com.apple.* C strings of __TEXT (bootstrap/notify/mach-service name candidates).
// Writes OUTDIR/<basename>.bin (concatenated text bytes) and .idx:
//   text <guest_start> <bin_offset> <size>
//   export <guest_addr> <name>
#include "shared_cache.hpp"
#include <cstdio>
#include <fstream>
#include <string>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    arm64metal::SharedCache cache;
    std::string out = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string path = argv[i];
        auto image = cache.guestImage(path);
        if (!image) { std::fprintf(stderr, "missing %s\n", path.c_str()); continue; }
        std::string base = path.substr(path.rfind('/') + 1);
        std::ofstream bin(out + "/" + base + ".bin", std::ios::binary);
        std::ofstream idx(out + "/" + base + ".idx");
        idx << "image " << path << "\n";
        std::uint64_t off = 0;
        for (const auto& [start, end] : image->text) {
            auto data = cache.bytes(start, end - start);
            bin.write(reinterpret_cast<const char*>(data.data()), data.size());
            idx << "text " << cache.guestAddress(start) << " " << off << " " << data.size() << "\n";
            off += data.size();
        }
        for (const auto& seg : image->segments) {
            if (seg.name != "__TEXT") continue;
            std::vector<std::uint8_t> data;
            try { data = cache.bytes(seg.address, seg.size); } catch (...) { continue; }
            std::string cur;
            for (auto c : data) {
                if (c >= 0x20 && c < 0x7f) { cur.push_back(static_cast<char>(c)); continue; }
                if (c == 0 && cur.size() >= 8 && cur.size() < 200 && cur.find("com.apple.") != std::string::npos)
                    idx << "str " << cur << "\n";
                cur.clear();
            }
        }
        for (const auto& [name, addr] : image->exports) idx << "export " << addr << " " << name << "\n";
        for (const auto& d : image->dependencies) idx << "dep " << d.name << "\n";
        std::fprintf(stderr, "%s text=%llu exports=%zu\n", base.c_str(), (unsigned long long)off, image->exports.size());
    }
}
