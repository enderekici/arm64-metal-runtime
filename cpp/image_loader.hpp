#pragma once

#include "guest_code_policy.hpp"
#include "guest_image_registry.hpp"
#include "macho.hpp"
#include "shared_cache.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arm64metal {

struct ImageDependency {
  std::string name;
  bool weak = false;
  bool reexported = false;
  std::uint32_t current_version = 0;
  bool upward = false;
  bool lazy_load = false;
  bool delayed_init = false;
};

struct LoadedImageDependency {
  ImageDependency declaration;
  std::optional<std::uint64_t> resolved_header;
};

struct LoadedImageGraphRecord {
  std::uint64_t guest_header = 0;
  std::string canonical_path;
  // Expanded loader search paths inherited by this image and its descendants.
  std::vector<std::string> rpaths;
  // Declaration order is preserved because ordinals index this vector.
  std::vector<LoadedImageDependency> dependencies;
  std::map<std::string, MachExport> exports;
  // Dynamic-image metadata captured for a later unload transaction. Startup
  // and shared-cache images leave this empty and remain pinned.
  std::size_t backing_offset = 0;
  std::size_t image_size = 0;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> mapped_ranges;
  std::vector<std::uint64_t> initializer_targets;
  std::vector<std::uint64_t> finalizer_targets;
  std::vector<std::uint64_t> binding_slots;
  std::vector<std::uint64_t> synthetic_import_stubs;
  std::vector<std::size_t> authentication_tokens;
  bool global_scope = false;
  bool dynamic_unload_candidate = false;
};

struct StagedNamedDependency {
  ImageDependency declaration;
  std::optional<std::uint64_t> existing_header;
  std::optional<std::size_t> staged_image_index;
};

struct StagedNamedBind {
  std::uint64_t guest_slot = 0;
  ChainedImport imported;
  std::optional<std::uint64_t> dependency_header;
  std::optional<std::uint64_t> resolved_target;
  std::optional<std::uint64_t> deferred_target;
  std::optional<std::string> required_cache_image;
  std::optional<ChainedAuthentication> authentication;
  std::optional<int> host_service_id;
  std::string resolved_name;
  bool is_stub = false;
  bool guest_memory_import = false;
  bool weak_null = false;
  bool unresolved = false;
  bool lazy = false;
  bool ignored_eager_lazy_binder = false;

  StagedNamedBind(std::uint64_t slot, ChainedImport import, bool stub)
      : guest_slot(slot), imported(std::move(import)), is_stub(stub) {}
};

struct StagedCacheSegment {
  std::int64_t logical_start;
  std::int64_t logical_end;
  SharedCache::Segment segment;
};

struct StagedCacheImage {
  std::string install_name;
  std::uint64_t guest_header = 0;
  std::int64_t slide = 0;
  SharedCache::GuestImage image;
  std::vector<StagedCacheSegment> segments;
  std::vector<std::uint64_t> initializer_targets;
  std::vector<SharedCache::Initializer> initializers;
  std::uint64_t segment_bytes = 0;
  bool already_mapped = false;
  // Startup eager reachability is distinct from whether constructors ran.
  bool startup_active = false;
  bool delayed_initialization = false;
};

struct GuestBuildVersion {
  std::uint32_t platform;
  std::uint32_t minimum;
  std::uint32_t sdk;
  // Raw SDK load-command value used by the program SDK scalar API.
  std::uint32_t sdk_load_command = 0;

  std::uint64_t token(bool query_sdk) const noexcept;
};

// Local image TLV layout. Address fields are image-relative offsets in MachImage
// and guest addresses in ExecutableImage::thread_local_images.
struct ThreadLocalImageMetadata {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> thunk_ranges;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> template_ranges;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> init_callback_ranges;
  std::uint64_t template_address = 0;
  std::uint64_t template_size = 0;
  std::uint64_t template_alignment = 1;
  bool all_zero_fill = true;
  bool from_shared_cache = false;
  // Cache builder format is not inferred from section geometry.
  bool thunk_encoding_verified = false;
  std::vector<std::uint64_t> init_callbacks;
};

struct StartupInitializerImage {
  std::uint64_t header = 0;
  std::string path;
  bool from_shared_cache = false;
  std::vector<std::uint64_t> local_targets;
  std::vector<SharedCache::Initializer> cached_targets;
};

struct CodeABIRegion {
  std::int64_t start;
  std::int64_t end;
  std::optional<bool> arm64e;
};

struct MappedImageSegment {
  std::uint64_t guest_header;
  std::int64_t start;
  std::int64_t end;
  std::string image_path;
  std::string segment_name;
  bool executable = false;
  bool data_segment = false;
};

struct RuntimeLocalImageBytes {
  std::uint64_t guest_header = 0;
  std::vector<std::uint8_t> bytes;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> executable_ranges;
};

struct MachMappedSegment {
  std::string name;
  std::uint64_t offset;
  std::uint64_t size;
  bool executable;
  bool data_segment;
};

struct ModeledGlobalExportProvenance {
  std::uint64_t guest_header;
  std::uint64_t guest_address;
  std::string canonical_path;
};

// Parsed local executable or dylib. All code ranges and fixup slots are offsets
// within the guest image; `preferred_base` remains the Mach-O unslid address.
struct MachImage {
  std::vector<std::uint8_t> memory;
  std::optional<std::size_t> entry;
  std::uint64_t preferred_base = 0;
  std::optional<bool> arm64e_code_abi;
  std::vector<std::uint8_t> uuid;
  bool restricted = false;
  std::optional<GuestBuildVersion> build_version;
  std::vector<std::pair<std::size_t, std::size_t>> code;
  std::vector<MachMappedSegment> mapped_segments;
  std::map<std::size_t, ChainedImport> stubs;
  std::map<std::size_t, ChainedImport> bindings;
  std::set<std::size_t> lazy_bind_locations;
  std::map<std::size_t, ChainedAuthentication> authentication;
  std::vector<std::pair<std::size_t, std::size_t>> writable_regions;
  std::vector<std::pair<std::size_t, std::size_t>> immutable_regions;
  std::vector<ImageDependency> dependencies;
  std::vector<std::string> rpaths;
  std::map<std::string, MachExport> exports;
  std::vector<std::size_t> initializers;
  std::vector<std::size_t> finalizers;
  std::optional<ThreadLocalImageMetadata> thread_locals;
  GuestCodePolicy code_policy;

  MachImage(const std::filesystem::path& path, bool executable, std::size_t placement,
            bool capture_finalizer_metadata = false);
};

struct StagedNamedImage {
  MachImage image;
  std::uint64_t guest_header = 0;
  std::uint64_t slide = 0;
  std::uint64_t guest_end = 0;
  std::size_t image_bytes = 0;
  std::string canonical_path;
  std::vector<std::string> rpaths;
  std::vector<StagedNamedDependency> dependencies;
  std::vector<std::uint64_t> initializer_targets;
  std::vector<std::uint64_t> finalizer_targets;
  std::vector<StagedNamedBind> binds;
  bool delayed_initialization = false;

  StagedNamedImage(MachImage parsed, std::uint64_t header, std::uint64_t end,
                   std::string path, std::vector<std::string> search_paths,
                   bool delayed = false)
      : image(std::move(parsed)), guest_header(header),
        slide(header - image.preferred_base), guest_end(end),
        image_bytes(image.memory.size()), canonical_path(std::move(path)),
        rpaths(std::move(search_paths)), delayed_initialization(delayed) {}
};

struct StagedNamedLoad {
  std::uint64_t requested_header = 0;
  std::string canonical_path;
  std::size_t total_image_bytes = 0;
  std::uint64_t total_cache_segment_bytes = 0;
  std::vector<StagedCacheImage> cache_images;
  // Empty when the requested image was already loaded. New entries are in
  // dependency-first order and can be published as one transaction.
  std::vector<StagedNamedImage> images;
};

struct CacheRegion {
  std::int64_t logical_start;
  std::int64_t logical_end;
  std::size_t backing;
  bool writable;
  bool readable = true;
  bool tpro = false;
};

struct DeferredCacheSegment {
  std::int64_t logical_start;
  std::int64_t logical_end;
  SharedCache::Segment segment;
};

struct CachedImageRange {
  std::int64_t logical_start;
  std::int64_t logical_end;
  std::int64_t header;
};

// Dependency graph and guest VM image layout. Cached system images are available
// only if `shared_cache` is true. Demand mode reserves cache segments and faults
// them into guest memory on access.
struct ExecutableImage {
  static constexpr std::size_t minimum_image_address = 16'384;
  static constexpr std::int64_t objc_mutation_invoke = (1LL << 48) - 4;

  std::vector<std::uint8_t> memory;
  std::size_t entry = 0;
  std::int64_t main_header = 0;
  std::vector<std::uint8_t> main_uuid;
  bool main_restricted = false;
  GuestCodePolicy main_code_policy;
  std::vector<std::pair<std::int64_t, std::int64_t>> text_regions;
  std::vector<CodeABIRegion> code_abi_regions;
  // Each actual mapped segment remains separate, so gaps between segments do
  // not resolve as belonging to an image (all ranges are half-open).
  std::vector<MappedImageSegment> mapped_image_segments;
  // Append-only guest code/data overlays published by the runner after a
  // successful named-load transaction. These do not resize the base backing.
  std::vector<RuntimeLocalImageBytes> runtime_local_images;
  std::vector<std::size_t> initializers;
  // Alternative ordered plan for an explicit consumer. Do not concatenate this
  // with `initializers`: local constructors are represented in both sequences.
  std::vector<StartupInitializerImage> startup_initializer_images;
  std::vector<SharedCache::Initializer> startup_libsystem_initializers;
  bool startup_initializer_metadata_collected = false;
  bool startup_initializer_upward_links_supported = false;
  bool startup_initializer_libsystem_is_guest_modeled = true;
  std::optional<std::int64_t> objc_bootstrap;
  std::unordered_map<std::uint64_t, std::uint64_t> image_slides;
  std::unordered_map<std::uint64_t, std::string> image_paths;
  // Startup RTLD_DEFAULT search order: main image, then breadth-first declared
  // dependency order. Prebound lazy-only images are excluded until activated.
  std::vector<std::uint64_t> global_scope_headers;
  // Dyld image enumeration order. Local startup images precede the loaded
  // shared-cache dependency closure in cache image-table order.
  std::vector<GuestLoadedImage> loaded_image_order;
  // Images eagerly reachable at startup, independent of constructor collection.
  std::set<std::uint64_t> startup_eager_headers;
  // Startup graph metadata for append-only staged loading. The records retain
  // canonical paths, declared dependency ordinals, and resolved image headers.
  std::vector<LoadedImageGraphRecord> loaded_image_graph;
  std::unordered_map<std::uint64_t, ThreadLocalImageMetadata> thread_local_images;
  std::unordered_map<std::string, ModeledGlobalExportProvenance>
      modeled_global_export_provenance;
  std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>>
      cached_export_addresses;
  std::unordered_map<std::string, std::vector<ImageDependency>> cached_image_dependencies;
  std::vector<CachedImageRange> cached_image_ranges;
  std::optional<GuestBuildVersion> build_version;
  std::unordered_map<std::int64_t, std::string> imports;
  std::unordered_map<std::int64_t, std::int64_t> guest_stubs;
  std::unordered_map<std::size_t, std::string> bindings;
  std::unordered_map<std::size_t, std::int64_t> function_bindings;
  std::unordered_map<std::size_t, ChainedAuthentication> authentication;
  std::vector<std::pair<std::int64_t, std::int64_t>> writable_regions;
  std::vector<std::pair<std::int64_t, std::int64_t>> immutable_regions;
  std::vector<CacheRegion> cache_regions;
  std::size_t linear_image_end = 0;
  std::optional<SharedCache> cache_reader;
  std::vector<DeferredCacheSegment> deferred_cache;
  std::unordered_map<std::size_t, std::string> deferred_bindings;
  std::optional<std::int64_t> cached_task_port;
  std::optional<std::int64_t> swift_prespecialized_data;
  std::optional<std::int64_t> cached_dispatch_main_queue;

  ExecutableImage(const std::filesystem::path& binary, bool shared_cache = false,
                  bool demand_cache = false,
                  const std::unordered_map<std::string, int>& host_service_ids = {},
                  const std::set<std::string>& guest_memory_imports = {},
                  bool collect_startup_initializer_metadata = false);

  bool cache_enabled() const noexcept;
  void activate_named_cache_reader();
  const SharedCache* metadata_cache_reader() const noexcept;
  StagedNamedLoad stage_named_load(const std::string& requested_path,
                                   std::uint64_t caller_header,
                                   std::uint64_t logical_placement,
                                   bool now = true) const;
  std::size_t cache_backing_start() const noexcept;
  std::size_t cache_backing_end() const noexcept;
  std::size_t mapping_backing_end() const noexcept;
  std::uint32_t link_time_version(const std::string& name) const noexcept;
  bool is_code(std::int64_t address) const noexcept;
  std::optional<bool> is_arm64e_code(std::int64_t pc) const noexcept;
  std::uint32_t instruction(std::int64_t address) const;
  std::optional<std::pair<std::int64_t, std::uint64_t>> lookup_guest_section(
      std::int64_t header, std::size_t kind) const;

 private:
  std::vector<ImageDependency> linked_libraries_;
  std::unordered_map<std::string, int> host_service_ids_;
  std::set<std::string> guest_memory_imports_;
  std::optional<SharedCache> named_load_cache_reader_;
};

}  // namespace arm64metal
