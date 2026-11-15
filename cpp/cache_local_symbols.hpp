#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "shared_cache.hpp"

namespace arm64metal {

// Looks up a symbol (exported or local) in a cached image's LC_SYMTAB and
// returns its guest address. Local symbols are not in the export trie.
std::optional<std::int64_t> cache_symtab_symbol(const SharedCache& cache,
                                                const std::string& path,
                                                const std::string& name);

}  // namespace arm64metal
