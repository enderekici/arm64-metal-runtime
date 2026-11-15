// CPU-only opcode sweep: runs decode_process over every __TEXT,__text word of
// the named Mach-O files and dyld shared-cache images and prints a histogram
// of encodings the process decoder rejects. No Metal device is touched.
//
// usage: arm64-metal-sweep [--scratch DIR] [--cache SUBSTRING]... [FILE...]
//   FILE            thin or universal ARM64 Mach-O (executable or dylib)
//   --cache SUB     every shared-cache image whose path contains SUB
//   --scratch DIR   where the disassembly helper files go (default: cwd)
//
// Words skipped as the runtime itself does: LC_DATA_IN_CODE ranges, BRK
// (lowered to SIGTRAP) and words with (w & 0xffe0001f) == 0 (UDF/zero padding).
// A rejected word is classified by llvm-objdump --mattr=+all: a real mnemonic
// is a gap; "udf"/"<unknown>" is undefined (data or padding in __text).
#include "macho.hpp"
#include "process_decoder.hpp"
#include "shared_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TextRange {
  std::uint64_t address;  // unslid VM address
  std::vector<std::uint8_t> bytes;
};

struct DataInCode {
  std::uint64_t address;  // unslid VM address
  std::uint32_t length;
};

struct Image {
  std::string name;
  std::vector<TextRange> text;
  std::vector<DataInCode> dice;
  std::string note;
};

using Reader = std::function<std::vector<std::uint8_t>(std::uint64_t, std::uint64_t)>;

std::uint64_t le(const std::vector<std::uint8_t>& b, std::size_t o, std::size_t w) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < w; ++i) v |= std::uint64_t(b.at(o + i)) << (8 * i);
  return v;
}

std::string fixed(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::string s(reinterpret_cast<const char*>(&b.at(o)), 16);
  return s.substr(0, s.find('\0'));
}

// Parse a mapped Mach-O header. `header` holds mach_header_64 + load commands.
// `readVm` reads by unslid VM address; `fileToVm` turns a linkedit file offset
// into an address (std::nullopt-like ~0 when not resolvable).
Image parse(const std::string& name, const std::vector<std::uint8_t>& header,
            const Reader& readVm, bool fileBacked,
            const std::vector<std::uint8_t>* file) {
  Image image{name, {}, {}, {}};
  if (le(header, 0, 4) != 0xfeedfacfU) throw std::runtime_error("not a 64-bit Mach-O: " + name);
  const auto ncmds = le(header, 16, 4);
  std::size_t off = 32;
  std::uint64_t textVm = 0, linkVm = 0, linkFile = 0;
  bool haveLink = false;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> textFileToVm;  // (file offset, VM)
  std::uint64_t diceOff = 0, diceSize = 0;
  for (std::uint64_t c = 0; c < ncmds; ++c) {
    const auto cmd = le(header, off, 4), size = le(header, off + 4, 4);
    if (cmd == 0x19) {
      const auto seg = fixed(header, off + 8);
      const auto vm = le(header, off + 24, 8), fo = le(header, off + 40, 8);
      if (seg == "__TEXT") textVm = vm;
      if (seg == "__LINKEDIT") { linkVm = vm; linkFile = fo; haveLink = true; }
      const auto nsects = le(header, off + 64, 4);
      for (std::uint64_t s = 0; s < nsects; ++s) {
        const auto so = off + 72 + s * 80;
        if (fixed(header, so) != "__text" || fixed(header, so + 16) != "__TEXT") continue;
        const auto addr = le(header, so + 32, 8), sz = le(header, so + 40, 8);
        const auto fileOff = le(header, so + 48, 4);
        TextRange r{addr, {}};
        textFileToVm.emplace_back(fileOff, addr);
        if (fileBacked) {
          if (fileOff + sz > file->size()) throw std::runtime_error("__text past EOF: " + name);
          r.bytes.assign(file->begin() + fileOff, file->begin() + fileOff + sz);
        } else {
          r.bytes = readVm(addr, sz);
        }
        image.text.push_back(std::move(r));
      }
    }
    if (cmd == 0x29) { diceOff = le(header, off + 8, 4); diceSize = le(header, off + 12, 4); }
    off += size;
  }
  if (diceSize != 0) {
    std::vector<std::uint8_t> table;
    if (fileBacked) {
      if (diceOff + diceSize > file->size()) throw std::runtime_error("LC_DATA_IN_CODE past EOF");
      table.assign(file->begin() + diceOff, file->begin() + diceOff + diceSize);
    } else if (haveLink && diceOff >= linkFile) {
      try { table = readVm(linkVm + (diceOff - linkFile), diceSize); }
      catch (...) { image.note = "LC_DATA_IN_CODE unreadable in cache"; }
    }
    for (std::size_t e = 0; e + 8 <= table.size(); e += 8) {
      // offset counts from the mach header. In a cache image the header VM is
      // the __TEXT vmaddr; in a file (also MH_OBJECT) map through __text.
      const auto offset = le(table, e, 4);
      std::uint64_t address = textVm + offset;
      if (fileBacked) {
        for (std::size_t t = 0; t < textFileToVm.size(); ++t) {
          const auto [fo, vm] = textFileToVm[t];
          if (offset >= fo && offset < fo + image.text[t].bytes.size()) address = vm + (offset - fo);
        }
      }
      image.dice.push_back({address, static_cast<std::uint32_t>(le(table, e + 4, 2))});
    }
  }
  return image;
}

struct Hit {
  long count = 0;
  std::string example;  // "image@0xaddr"
  std::string message;  // first decoder message
};

struct Group {
  long count = 0;
  std::uint32_t andBits = ~0u, orBits = 0;
  std::map<std::uint32_t, long> words;
  std::string example, message;
};

std::vector<std::string> disassemble(const std::vector<std::uint32_t>& words,
                                     const std::string& scratch) {
  const auto s = scratch + "/sweep-disasm.s", o = scratch + "/sweep-disasm.o";
  {
    std::ofstream out(s);
    out << ".text\n";
    for (auto w : words) {
      char line[32];
      std::snprintf(line, sizeof line, ".long 0x%08x\n", w);
      out << line;
    }
  }
  const auto assemble = "xcrun clang -target arm64e-apple-macos -c '" + s + "' -o '" + o + "'";
  if (std::system(assemble.c_str()) != 0) throw std::runtime_error("assembling disassembly probe failed");
  const auto cmd = "xcrun llvm-objdump -d --no-show-raw-insn --mattr=+all '" + o + "'";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) throw std::runtime_error("llvm-objdump failed");
  std::vector<std::string> result(words.size(), "<unknown>");
  char buf[512];
  while (std::fgets(buf, sizeof buf, pipe.get())) {
    char* colon = std::strchr(buf, ':');
    if (!colon) continue;
    char* end = nullptr;
    const auto at = std::strtoull(buf, &end, 16);
    if (end != colon || at % 4 != 0 || at / 4 >= words.size()) continue;
    std::string text(colon + 1);
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    text = text.substr(first);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    for (auto& ch : text) if (ch == '\t') ch = ' ';
    result[at / 4] = text;
  }
  std::remove(s.c_str());
  std::remove(o.c_str());
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scratch = ".", acceptedPath;
  std::vector<std::string> files, cacheSubs;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scratch" && i + 1 < argc) scratch = argv[++i];
    else if (a == "--cache" && i + 1 < argc) cacheSubs.push_back(argv[++i]);
    else if (a == "--accepted" && i + 1 < argc) acceptedPath = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::fprintf(stderr, "usage: arm64-metal-sweep [--scratch DIR] [--accepted OUT] [--cache SUBSTRING]... [FILE...]\n");
      return 0;
    } else files.push_back(a);
  }
  std::vector<Image> images;
  for (const auto& path : files) {
    arm64metal::MachO macho{std::filesystem::path(path), true};
    const auto& bytes = macho.bytes();
    images.push_back(parse(path, bytes, nullptr, true, &bytes));
  }
  std::unique_ptr<arm64metal::SharedCache> cache;
  if (!cacheSubs.empty()) {
    cache = std::make_unique<arm64metal::SharedCache>();
    std::map<std::uint64_t, std::string> chosen;  // dedupe aliases by header
    for (const auto& [path, address] : cache->images()) {
      for (const auto& sub : cacheSubs) {
        if (path.find(sub) == std::string::npos) continue;
        auto it = chosen.find(address);
        if (it == chosen.end() || path < it->second) chosen[address] = path;
      }
    }
    Reader reader = [&](std::uint64_t a, std::uint64_t n) { return cache->bytes(a, n); };
    for (const auto& [address, path] : chosen) {
      const auto head = cache->bytes(address, 32);
      const auto sizeofcmds = le(head, 20, 4);
      images.push_back(parse("cache:" + path, cache->bytes(address, 32 + sizeofcmds), reader,
                             false, nullptr));
    }
  }

  std::map<std::uint32_t, Hit> rejected;
  std::map<std::uint32_t, long> accepted;  // only filled with --accepted
  long totalWords = 0, totalRejected = 0, totalDice = 0, totalBrk = 0, totalZero = 0;
  std::vector<std::string> perImage;
  for (const auto& image : images) {
    long words = 0, bad = 0, diceWords = 0;
    for (const auto& range : image.text) {
      for (std::size_t o = 0; o + 4 <= range.bytes.size(); o += 4) {
        const auto address = range.address + o;
        bool inDice = false;
        for (const auto& d : image.dice) {
          if (address >= d.address && address < d.address + d.length) { inDice = true; break; }
        }
        if (inDice) { ++diceWords; continue; }
        std::uint32_t w;
        std::memcpy(&w, &range.bytes[o], 4);
        ++words;
        if ((w & 0xffe0001fU) == 0xd4200000U) { ++totalBrk; continue; }
        if ((w & 0xffe0001fU) == 0) { ++totalZero; continue; }
        try {
          // Placed PC is irrelevant to which encodings decode; keep it in range.
          arm64metal::decode_process(w, static_cast<std::int64_t>(0x100000000ULL + (address & 0xffffffffULL)),
                                     {}, {}, true);
          if (!acceptedPath.empty()) ++accepted[w];
        } catch (const std::exception& e) {
          ++bad;
          auto& hit = rejected[w];
          if (hit.count++ == 0) {
            char ex[64];
            std::snprintf(ex, sizeof ex, "@0x%llx", static_cast<unsigned long long>(address));
            hit.example = image.name + ex;
            hit.message = e.what();
          }
        }
      }
    }
    totalWords += words; totalRejected += bad; totalDice += diceWords;
    char line[1024];
    std::snprintf(line, sizeof line, "#   %-70s text_words=%-9ld rejected=%-7ld dice_words=%ld%s%s",
                  image.name.c_str(), words, bad, diceWords, image.note.empty() ? "" : " note=",
                  image.note.c_str());
    perImage.push_back(line);
  }

  std::vector<std::uint32_t> distinct;
  for (const auto& [w, _] : rejected) distinct.push_back(w);
  std::vector<std::string> dis;
  for (std::size_t i = 0; i < distinct.size(); i += 20000) {
    std::vector<std::uint32_t> chunk(distinct.begin() + i,
                                     distinct.begin() + std::min(distinct.size(), i + 20000));
    auto part = disassemble(chunk, scratch);
    dis.insert(dis.end(), part.begin(), part.end());
  }

  std::map<std::string, Group> groups;
  long undefinedCount = 0, undefinedDistinct = 0;
  for (std::size_t i = 0; i < distinct.size(); ++i) {
    const auto w = distinct[i];
    const auto& hit = rejected[w];
    std::string mnemonic = dis[i].substr(0, dis[i].find(' '));
    if (mnemonic == "msr" || mnemonic == "mrs") {
      // Group system-register moves by register, not by the bare mnemonic.
      std::string ops = dis[i].substr(mnemonic.size());
      const auto comma = ops.find(',');
      std::string reg = mnemonic == "msr" ? ops.substr(0, comma) : ops.substr(comma + 1);
      reg.erase(0, reg.find_first_not_of(' '));
      reg = reg.substr(0, reg.find(' '));
      mnemonic += " " + reg;
    }
    if (mnemonic == "udf" || mnemonic == "<unknown>" || mnemonic.empty() || mnemonic == ".long") {
      undefinedCount += hit.count; ++undefinedDistinct;
      mnemonic = "(undefined:" + (mnemonic.empty() ? std::string("<unknown>") : mnemonic) + ")";
    }
    auto& g = groups[mnemonic];
    g.count += hit.count;
    g.andBits &= w;
    g.orBits |= w;
    g.words[w] = hit.count;
    if (g.example.empty() || hit.count > g.words[std::stoul(g.example.substr(0, 8), nullptr, 16)]) {
      char head[16];
      std::snprintf(head, sizeof head, "%08x", w);
      g.example = std::string(head) + " " + dis[i] + " " + hit.example;
      g.message = hit.message;
    }
  }
  std::vector<std::pair<std::string, const Group*>> order;
  for (const auto& [k, g] : groups) order.emplace_back(k, &g);
  std::stable_sort(order.begin(), order.end(),
                   [](const auto& a, const auto& b) { return a.second->count > b.second->count; });

  std::printf("# arm64-metal opcode sweep: decode_process(cached=true) over __TEXT,__text\n");
  std::printf("# skipped: LC_DATA_IN_CODE ranges, BRK (SIGTRAP path), (w & 0xffe0001f)==0 (UDF/zero)\n");
  std::printf("# mnemonic: llvm-objdump --mattr=+all (Apple syntax); mask/match = bits constant across\n");
  std::printf("#   every rejected word in the group (mask) and their value (match)\n");
  std::printf("# lists REJECTED words only: an encoding the decoder accepts but lowers wrongly is\n");
  std::printf("#   invisible here (use --accepted to audit accepted words by mnemonic)\n");
  std::printf("# totals: images=%zu text_words=%ld rejected=%ld (%.4f%%) distinct=%zu dice_words=%ld brk=%ld zero=%ld\n",
              images.size(), totalWords, totalRejected,
              totalWords ? 100.0 * totalRejected / totalWords : 0.0, distinct.size(), totalDice,
              totalBrk, totalZero);
  std::printf("# undefined (no mnemonic: data/padding): count=%ld distinct=%ld\n", undefinedCount,
              undefinedDistinct);
  std::printf("# per image:\n");
  for (const auto& l : perImage) std::printf("%s\n", l.c_str());
  std::printf("#\n# count\tdistinct\tmask\tmatch\tmnemonic\texample(word disasm image@addr)\tdecoder message\n");
  for (const auto& [k, g] : order) {
    const auto mask = ~(g->andBits ^ g->orBits);
    std::printf("%ld\t%zu\t0x%08x\t0x%08x\t%s\t%s\t%s\n", g->count, g->words.size(), mask,
                g->andBits & mask, k.c_str(), g->example.c_str(), g->message.c_str());
  }
  if (!acceptedPath.empty()) {
    std::vector<std::uint32_t> words;
    for (const auto& [w, _] : accepted) words.push_back(w);
    std::ofstream out(acceptedPath);
    for (std::size_t i = 0; i < words.size(); i += 20000) {
      std::vector<std::uint32_t> chunk(words.begin() + i,
                                       words.begin() + std::min(words.size(), i + 20000));
      const auto part = disassemble(chunk, scratch);
      for (std::size_t j = 0; j < chunk.size(); ++j) {
        char head[16];
        std::snprintf(head, sizeof head, "%08x", chunk[j]);
        out << head << '\t' << accepted[chunk[j]] << '\t' << part[j] << '\n';
      }
    }
  }
  return 0;
}
