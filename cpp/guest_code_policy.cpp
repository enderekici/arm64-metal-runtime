#include "guest_code_policy.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecCode.h>
#include <Security/SecStaticCode.h>
#include <Security/SecTrust.h>
#include <copyfile.h>
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <limits.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unistd.h>

namespace arm64metal {
namespace {

class CfRelease {
 public:
  template <typename T>
  void operator()(T* value) const noexcept {
    if (value != nullptr) CFRelease(value);
  }
};

template <typename T>
using CfPtr = std::unique_ptr<T, CfRelease>;

CfPtr<const __CFURL> file_url(const std::filesystem::path& path, bool directory) {
  const auto path_string = path.string();
  return CfPtr<const __CFURL>(CFURLCreateFromFileSystemRepresentation(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path_string.data()),
      static_cast<CFIndex>(path_string.size()), directory));
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  return output.good();
}

bool copy_code_file(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
  return copyfile(source.c_str(), destination.c_str(), nullptr,
                  COPYFILE_DATA | COPYFILE_METADATA | COPYFILE_NOFOLLOW_SRC) == 0;
}

bool copy_bundle_signature_context(const std::filesystem::path& source_root,
                                   const std::filesystem::path& source_executable,
                                   const std::filesystem::path& snapshot_root) {
  std::error_code error;
  std::filesystem::create_directories(snapshot_root, error);
  if (error) return false;
  auto ancestor = source_executable.parent_path();
  while (true) {
    for (const auto* name : {"Info.plist", "_CodeSignature"}) {
      const auto source = ancestor / name;
      if (!std::filesystem::exists(source, error)) {
        if (error) return false;
        continue;
      }
      const auto relative = source.lexically_relative(source_root);
      const auto destination = snapshot_root / relative;
      std::filesystem::create_directories(destination.parent_path(), error);
      if (error) return false;
      if (std::filesystem::is_directory(source, error)) {
        if (error || copyfile(source.c_str(), destination.c_str(), nullptr,
                              COPYFILE_DATA | COPYFILE_RECURSIVE |
                                  COPYFILE_NOFOLLOW_SRC) != 0) return false;
      } else if (error || !copy_code_file(source, destination)) {
        return false;
      }
    }
    if (ancestor == source_root) break;
    ancestor = ancestor.parent_path();
  }
  return !error;
}

bool dictionary_boolean(CFDictionaryRef dictionary, CFStringRef key) {
  if (dictionary == nullptr) return false;
  const auto value = static_cast<CFTypeRef>(CFDictionaryGetValue(dictionary, key));
  return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() &&
         CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

bool dictionary_uint32(CFDictionaryRef dictionary, CFStringRef key,
                       std::uint32_t& result) {
  if (dictionary == nullptr) return false;
  const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
  if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
  SInt32 number = 0;
  if (!CFNumberGetValue(value, kCFNumberSInt32Type, &number)) return false;
  result = static_cast<std::uint32_t>(number);
  return true;
}

std::uint32_t read_little_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
      (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
      (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
      (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_big_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
      (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
      (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
      static_cast<std::uint32_t>(bytes[offset + 3]);
}


struct DerElement {
  std::uint8_t tag = 0;
  const std::uint8_t* content = nullptr;
  std::size_t length = 0;
  std::size_t total = 0;
};

bool read_der_element(const std::uint8_t* data, std::size_t size, DerElement& element) {
  if (size < 2) return false;
  element.tag = data[0];
  std::size_t length = data[1];
  std::size_t header = 2;
  if ((length & 0x80U) != 0) {
    const auto count = length & 0x7fU;
    if (count == 0 || count > 4 || size < 2 + count) return false;
    length = 0;
    for (std::size_t index = 0; index < count; ++index) length = (length << 8) | data[2 + index];
    header += count;
  }
  if (length > size - header) return false;
  element.content = data + header;
  element.length = length;
  element.total = header + length;
  return true;
}

void append_xml_text(std::string& out, const std::uint8_t* data, std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    const char c = static_cast<char>(data[index]);
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
}

bool der_value_to_xml(const DerElement& value, std::string& out, int depth);

bool der_pairs_to_xml(const std::uint8_t* data, std::size_t size, std::string& out, int depth) {
  out += "<dict>";
  while (size != 0) {
    DerElement pair;
    if (!read_der_element(data, size, pair) || pair.tag != 0x30) return false;
    DerElement key;
    if (!read_der_element(pair.content, pair.length, key) || key.tag != 0x0c) return false;
    DerElement value;
    if (!read_der_element(pair.content + key.total, pair.length - key.total, value) ||
        key.total + value.total != pair.length) return false;
    out += "<key>";
    append_xml_text(out, key.content, key.length);
    out += "</key>";
    if (!der_value_to_xml(value, out, depth + 1)) return false;
    data += pair.total;
    size -= pair.total;
  }
  out += "</dict>";
  return true;
}

bool der_value_to_xml(const DerElement& value, std::string& out, int depth) {
  if (depth > 32) return false;
  switch (value.tag) {
    case 0x01:
      if (value.length != 1) return false;
      out += value.content[0] != 0 ? "<true/>" : "<false/>";
      return true;
    case 0x02: {
      if (value.length == 0 || value.length > 8) return false;
      std::int64_t number = (value.content[0] & 0x80U) != 0 ? -1 : 0;
      for (std::size_t index = 0; index < value.length; ++index) {
        number = static_cast<std::int64_t>((static_cast<std::uint64_t>(number) << 8) |
                                           value.content[index]);
      }
      out += "<integer>" + std::to_string(number) + "</integer>";
      return true;
    }
    case 0x0c:
      out += "<string>";
      append_xml_text(out, value.content, value.length);
      out += "</string>";
      return true;
    case 0x30: {
      out += "<array>";
      const auto* cursor = value.content;
      auto remaining = value.length;
      while (remaining != 0) {
        DerElement element;
        if (!read_der_element(cursor, remaining, element) ||
            !der_value_to_xml(element, out, depth + 1)) return false;
        cursor += element.total;
        remaining -= element.total;
      }
      out += "</array>";
      return true;
    }
    case 0x31:
      return der_pairs_to_xml(value.content, value.length, out, depth);
    default:
      return false;
  }
}

// The kernel serialises the DER entitlements as canonical XML (sorted keys, no
// whitespace) with a trailing NUL counted in the blob length.
bool der_entitlements_to_xml_blob(const std::vector<std::uint8_t>& der,
                                  std::vector<std::uint8_t>& blob) {
  if (der.size() < 10) return false;
  DerElement root;
  if (!read_der_element(der.data() + 8, der.size() - 8, root) || root.tag != 0x70) return false;
  DerElement version;
  if (!read_der_element(root.content, root.length, version) || version.tag != 0x02) return false;
  DerElement dictionary;
  if (!read_der_element(root.content + version.total, root.length - version.total, dictionary) ||
      dictionary.tag != 0xb0) return false;
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST "
      "1.0//EN\" \"https://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\">";
  if (!der_pairs_to_xml(dictionary.content, dictionary.length, xml, 0)) return false;
  xml += "</plist>";
  const auto total = static_cast<std::uint32_t>(8 + xml.size() + 1);
  blob.assign({0xfa, 0xde, 0x71, 0x71, static_cast<std::uint8_t>(total >> 24),
               static_cast<std::uint8_t>(total >> 16), static_cast<std::uint8_t>(total >> 8),
               static_cast<std::uint8_t>(total)});
  blob.insert(blob.end(), xml.begin(), xml.end());
  blob.push_back(0);
  return true;
}

bool capture_entitlement_blobs(const std::vector<std::uint8_t>& image,
                               GuestCodePolicy& policy) {
  constexpr std::uint32_t macho64Magic = 0xfeedfacf;
  constexpr std::uint32_t lcCodeSignature = 0x1d;
  constexpr std::uint32_t embeddedSignatureMagic = 0xfade0cc0;
  constexpr std::uint32_t xmlEntitlementsMagic = 0xfade7171;
  constexpr std::uint32_t derEntitlementsMagic = 0xfade7172;
  constexpr std::uint32_t xmlEntitlementsSlot = 5;
  constexpr std::uint32_t derEntitlementsSlot = 7;
  if (image.size() < 32 || read_little_u32(image, 0) != macho64Magic) return false;
  const auto commandCount = read_little_u32(image, 16);
  const auto commandBytes = read_little_u32(image, 20);
  if (commandBytes > image.size() - 32) return false;
  const auto commandEnd = std::size_t{32} + commandBytes;
  std::size_t commandOffset = 32;
  std::optional<std::pair<std::size_t, std::size_t>> codeSignature;
  for (std::uint32_t index = 0; index < commandCount; ++index) {
    if (commandOffset > commandEnd || commandEnd - commandOffset < 8) return false;
    const auto command = read_little_u32(image, commandOffset);
    const auto commandSize = read_little_u32(image, commandOffset + 4);
    if (commandSize < 8 || commandSize > commandEnd - commandOffset) return false;
    if (command == lcCodeSignature) {
      if (commandSize < 16 || codeSignature) return false;
      const auto dataOffset = read_little_u32(image, commandOffset + 8);
      const auto dataSize = read_little_u32(image, commandOffset + 12);
      if (dataOffset > image.size() || dataSize > image.size() - dataOffset) return false;
      codeSignature = std::pair{static_cast<std::size_t>(dataOffset),
                                static_cast<std::size_t>(dataSize)};
    }
    commandOffset += commandSize;
  }
  if (commandOffset != commandEnd) return false;
  if (!codeSignature) {
    policy.entitlements_blob_metadata_valid = true;
    return true;
  }
  const auto signatureOffset = codeSignature->first;
  const auto signatureSize = codeSignature->second;
  policy.code_signature.assign(
      image.begin() + static_cast<std::ptrdiff_t>(signatureOffset),
      image.begin() + static_cast<std::ptrdiff_t>(signatureOffset + signatureSize));
  if (signatureSize < 12 || read_big_u32(image, signatureOffset) !=
          embeddedSignatureMagic) return false;
  const auto superblobLength = read_big_u32(image, signatureOffset + 4);
  const auto blobCount = read_big_u32(image, signatureOffset + 8);
  if (superblobLength > signatureSize || superblobLength < 12 ||
      blobCount > (superblobLength - 12) / 8) return false;
  std::optional<std::size_t> xmlBlobOffset;
  std::optional<std::size_t> derBlobOffset;
  std::vector<std::pair<std::uint32_t, std::size_t>> directories;
  for (std::uint32_t index = 0; index < blobCount; ++index) {
    const auto indexOffset = signatureOffset + 12 + static_cast<std::size_t>(index) * 8;
    const auto slot = read_big_u32(image, indexOffset);
    const auto blobOffset = read_big_u32(image, indexOffset + 4);
    if (blobOffset < 12 + blobCount * 8 || blobOffset > superblobLength ||
        superblobLength - blobOffset < 8) return false;
    if (slot == 0 || (slot >= 0x1000 && slot < 0x1005)) {
      directories.emplace_back(slot, signatureOffset + blobOffset);
    }
    if (slot == xmlEntitlementsSlot) {
      if (xmlBlobOffset) return false;
      xmlBlobOffset = signatureOffset + blobOffset;
    } else if (slot == derEntitlementsSlot) {
      if (derBlobOffset) return false;
      derBlobOffset = signatureOffset + blobOffset;
    }
  }
  const auto validateBlob = [&](std::size_t offset, std::uint32_t expectedMagic,
                                std::size_t& length) {
    const auto magic = read_big_u32(image, offset);
    const auto blobLength = read_big_u32(image, offset + 4);
    const auto relativeOffset = offset - signatureOffset;
    if (magic != expectedMagic || blobLength < 8 || relativeOffset > superblobLength ||
        blobLength > superblobLength - relativeOffset) return false;
    length = blobLength;
    return true;
  };
  if (xmlBlobOffset) {
    std::size_t ignoredLength = 0;
    if (!validateBlob(*xmlBlobOffset, xmlEntitlementsMagic, ignoredLength)) return false;
    policy.has_xml_entitlements = true;
  }
  if (derBlobOffset) {
    std::size_t length = 0;
    if (!validateBlob(*derBlobOffset, derEntitlementsMagic, length)) return false;
    policy.der_entitlements.assign(image.begin() + static_cast<std::ptrdiff_t>(*derBlobOffset),
        image.begin() + static_cast<std::ptrdiff_t>(*derBlobOffset + length));
  }
  if (!policy.der_entitlements.empty()) {
    if (!der_entitlements_to_xml_blob(policy.der_entitlements, policy.xml_entitlements)) {
      policy.xml_entitlements.clear();
      policy.xml_serialisation_unsupported = true;
    }
  } else if (xmlBlobOffset) {
    const auto length = read_big_u32(image, *xmlBlobOffset + 4);
    policy.xml_entitlements.assign(
        image.begin() + static_cast<std::ptrdiff_t>(*xmlBlobOffset),
        image.begin() + static_cast<std::ptrdiff_t>(*xmlBlobOffset + length));
  }
  constexpr std::uint32_t codeDirectoryMagic = 0xfade0c02;
  constexpr std::size_t codeDirectoryHeader = 48;
  std::optional<std::size_t> bestDirectory;
  std::uint8_t bestHashType = 0;
  for (const auto& [slot, offset] : directories) {
    (void)slot;
    if (read_big_u32(image, offset) != codeDirectoryMagic) continue;
    const auto length = read_big_u32(image, offset + 4);
    const auto relativeOffset = offset - signatureOffset;
    if (length < codeDirectoryHeader || relativeOffset > superblobLength ||
        length > superblobLength - relativeOffset) continue;
    const auto hashType = image[offset + 37];
    if (hashType < 1 || hashType > 4) continue;
    if (!bestDirectory || hashType > bestHashType) {
      bestDirectory = offset;
      bestHashType = hashType;
    }
  }
  if (bestDirectory) {
    const auto offset = *bestDirectory;
    const auto length = read_big_u32(image, offset + 4);
    std::array<std::uint8_t, CC_SHA512_DIGEST_LENGTH> digest{};
    if (bestHashType == 1) {
      CC_SHA1(image.data() + offset, length, digest.data());
    } else if (bestHashType == 4) {
      CC_SHA384(image.data() + offset, length, digest.data());
    } else {
      CC_SHA256(image.data() + offset, length, digest.data());
    }
    std::copy_n(digest.begin(), policy.cdhash.size(), policy.cdhash.begin());
    policy.has_cdhash = true;
    const auto version = read_big_u32(image, offset + 8);
    if (version >= 0x20200 && length >= 52) {
      const auto teamOffset = read_big_u32(image, offset + 48);
      if (teamOffset != 0 && teamOffset < length) {
        const auto* begin = reinterpret_cast<const char*>(image.data() + offset + teamOffset);
        const auto limit = length - teamOffset;
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, limit));
        if (end != nullptr) policy.team_id.assign(begin, end);
      }
    }
  }
  policy.entitlements_blob_metadata_valid = true;
  return true;
}

std::string dictionary_string(CFDictionaryRef dictionary, CFStringRef key) {
  if (dictionary == nullptr) return {};
  const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key));
  if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) return {};
  const auto length = CFStringGetLength(value);
  const auto capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
  if (capacity < 0 || capacity >= std::numeric_limits<CFIndex>::max()) return {};
  std::string result(static_cast<std::size_t>(capacity) + 1, '\0');
  if (!CFStringGetCString(value, result.data(), static_cast<CFIndex>(result.size()),
                          kCFStringEncodingUTF8)) return {};
  result.resize(std::char_traits<char>::length(result.c_str()));
  return result;
}

}  // namespace

GuestCodePolicy guest_code_policy(const std::filesystem::path& executable,
                                  const std::vector<std::uint8_t>& selected_image) {
  GuestCodePolicy result;
  CC_SHA256(selected_image.data(), static_cast<CC_LONG>(selected_image.size()),
            result.image_sha256.data());
  if (selected_image.empty() || selected_image.size() > UINT32_MAX) return result;

  char temporary[] = "/tmp/arm64-metal-code-policy.XXXXXX";
  char* temporary_directory = mkdtemp(temporary);
  if (temporary_directory == nullptr) return result;
  const std::filesystem::path temp_root(temporary_directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{temp_root};

  const auto source_url = file_url(executable, false);
  if (!source_url) return result;
  SecStaticCodeRef raw_source = nullptr;
  result.security_status = SecStaticCodeCreateWithPath(
      source_url.get(), kSecCSDefaultFlags, &raw_source);
  CfPtr<const __SecCode> source(raw_source);
  if (result.security_status != errSecSuccess || !source) return result;

  CFURLRef raw_root_url = nullptr;
  result.security_status = SecCodeCopyPath(
      const_cast<SecCodeRef>(source.get()), kSecCSDefaultFlags, &raw_root_url);
  CfPtr<const __CFURL> root_url(raw_root_url);
  if (result.security_status != errSecSuccess || !root_url) return result;
  CfPtr<const __CFString> root_string(
      CFURLCopyFileSystemPath(root_url.get(), kCFURLPOSIXPathStyle));
  if (!root_string) return result;
  char root_buffer[PATH_MAX]{};
  if (!CFStringGetCString(root_string.get(), root_buffer, sizeof(root_buffer),
                          kCFStringEncodingUTF8)) return result;

  std::error_code error;
  const auto source_root = std::filesystem::canonical(root_buffer, error);
  if (error) return result;
  const auto source_executable = std::filesystem::canonical(executable, error);
  if (error) return result;
  const bool bundle = std::filesystem::is_directory(source_root, error);
  if (error) return result;

  std::filesystem::path snapshot_root = temp_root / source_root.filename();
  std::filesystem::path snapshot_executable;
  if (bundle) {
    const auto relative = source_executable.lexically_relative(source_root);
    if (relative.empty() || relative.is_absolute()) return result;
    for (const auto& component : relative) if (component == "..") return result;
    if (!copy_bundle_signature_context(source_root, source_executable, snapshot_root)) {
      return result;
    }
    snapshot_executable = snapshot_root / relative;
    std::filesystem::create_directories(snapshot_executable.parent_path(), error);
    if (error || !copy_code_file(source_executable, snapshot_executable)) return result;
  } else {
    snapshot_root = temp_root / source_root.filename();
    snapshot_executable = snapshot_root;
    if (!copy_code_file(source_executable, snapshot_executable)) return result;
  }
  if (!std::filesystem::is_regular_file(snapshot_executable, error) || error ||
      !write_file(snapshot_executable, selected_image) ||
      read_file(snapshot_executable) != selected_image) return result;

  const auto snapshot_url = file_url(snapshot_root, bundle);
  if (!snapshot_url) return result;
  SecStaticCodeRef raw_snapshot = nullptr;
  result.security_status = SecStaticCodeCreateWithPath(
      snapshot_url.get(), kSecCSDefaultFlags, &raw_snapshot);
  CfPtr<const __SecCode> snapshot(raw_snapshot);
  if (result.security_status != errSecSuccess || !snapshot) return result;

  // Validate the signature and executable before trusting entitlement metadata.
  // Resource validation is disabled intentionally: it is neither needed to
  // authenticate the main executable nor stable for platform-signed bundles.
  result.security_status = SecStaticCodeCheckValidity(
      snapshot.get(), kSecCSStrictValidate | kSecCSDoNotValidateResources, nullptr);
  if (result.security_status != errSecSuccess) {
    result.validation = GuestCodeValidation::invalid;
    return result;
  }
  if (read_file(snapshot_executable) != selected_image) {
    result.security_status = errSecCSBadResource;
    result.validation = GuestCodeValidation::invalid;
    return result;
  }

  CFDictionaryRef raw_information = nullptr;
  result.security_status = SecCodeCopySigningInformation(
      const_cast<SecCodeRef>(snapshot.get()), kSecCSSigningInformation,
      &raw_information);
  CfPtr<const __CFDictionary> information(raw_information);
  if (result.security_status != errSecSuccess || !information) return result;

  result.validation = GuestCodeValidation::valid;
  result.identity = dictionary_string(information.get(), kSecCodeInfoIdentifier);
  if (!capture_entitlement_blobs(selected_image, result)) {
    result.entitlements_blob_metadata_valid = false;
  }
  (void)dictionary_uint32(information.get(), kSecCodeInfoFlags, result.signing_flags);
  result.hardened_runtime = (result.signing_flags & 0x00010000U) != 0;
  result.ad_hoc = (result.signing_flags & 0x00000002U) != 0;
  result.allow_jit = dictionary_boolean(
      static_cast<CFDictionaryRef>(CFDictionaryGetValue(
          information.get(), kSecCodeInfoEntitlementsDict)),
      CFSTR("com.apple.security.cs.allow-jit"));
  const auto trust = const_cast<SecTrustRef>(static_cast<const __SecTrust*>(
      CFDictionaryGetValue(information.get(), kSecCodeInfoTrust)));
  if (trust != nullptr && CFGetTypeID(trust) == SecTrustGetTypeID()) {
    result.trusted_signer = SecTrustEvaluateWithError(trust, nullptr);
  }
  // This models guest entitlement eligibility only. It never changes the
  // emulator's own code-signing entitlements or host VM protections.
  result.map_jit_eligible = !result.hardened_runtime || result.allow_jit;
  return result;
}

}  // namespace arm64metal
