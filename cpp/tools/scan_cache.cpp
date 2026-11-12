// Offline shared-cache opcode gap scanner. Not part of the runtime build.
// usage: scan-cache [--all | --list FILE | IMAGE_SUBSTRING...] [--exclude SUBSTRING]... > out.tsv
// Output lines:
//   I <image> <words> <unsupported> <other_errors>
//   W <word-hex> <total> <image>:<count>,...   (top images per word)
#include "process_decoder.hpp"
#include "shared_cache.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    arm64metal::SharedCache cache;
    std::vector<std::string> names;
    std::vector<std::string> subs, excl;
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--all") all = true;
        else if (a == "--exclude" && i + 1 < argc) excl.push_back(argv[++i]);
        else if (a == "--list" && i + 1 < argc) {
            std::ifstream f(argv[++i]);
            for (std::string l; std::getline(f, l);) if (!l.empty()) subs.push_back(l);
        } else subs.push_back(a);
    }
    for (const auto& [path, addr] : cache.images()) {
        bool take = all;
        for (const auto& s : subs) if (path.find(s) != std::string::npos) take = true;
        for (const auto& s : excl) if (path.find(s) != std::string::npos) take = false;
        if (take) names.push_back(path);
    }
    std::sort(names.begin(), names.end());
    std::map<std::uint32_t, std::map<std::string, long>> bad;
    long grand = 0, grandBad = 0;
    for (const auto& name : names) {
        auto image = cache.guestImage(name);
        if (!image) { std::fprintf(stderr, "skip %s\n", name.c_str()); continue; }
        long words = 0, unsup = 0, other = 0;
        for (const auto& [start, end] : image->text) {
            std::vector<std::uint8_t> data;
            try { data = cache.bytes(start, end - start); } catch (...) { ++other; continue; }
            for (std::size_t o = 0; o + 4 <= data.size(); o += 4) {
                std::uint32_t w;
                std::memcpy(&w, &data[o], 4);
                ++words;
                try {
                    arm64metal::decode_process(w, static_cast<std::int64_t>(0x10000 + start + o), {}, {}, true);
                } catch (const std::exception& e) {
                    if (std::strncmp(e.what(), "Unsupported", 11) != 0) { ++other; continue; }
                    ++unsup;
                    ++bad[w][name];
                }
            }
        }
        grand += words; grandBad += unsup;
        std::printf("I\t%s\t%ld\t%ld\t%ld\n", name.c_str(), words, unsup, other);
        std::fflush(stdout);
    }
    std::printf("T\t%ld\t%ld\n", grand, grandBad);
    for (const auto& [w, m] : bad) {
        long total = 0;
        std::vector<std::pair<long, std::string>> v;
        for (const auto& [n, c] : m) { total += c; v.emplace_back(c, n); }
        std::sort(v.rbegin(), v.rend());
        std::printf("W\t%08x\t%ld\t%zu", w, total, m.size());
        for (std::size_t i = 0; i < v.size() && i < 3; ++i) std::printf("\t%s:%ld", v[i].second.c_str(), v[i].first);
        std::printf("\n");
    }
}
