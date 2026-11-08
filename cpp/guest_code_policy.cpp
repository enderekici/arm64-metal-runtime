#include "guest_code_policy.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecCode.h>
#include <Security/SecStaticCode.h>
#include <Security/SecTrust.h>
#include <copyfile.h>
#include <CommonCrypto/CommonDigest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits.h>
#include <memory>
#include <string>
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
    if (copyfile(source_root.c_str(), snapshot_root.c_str(), nullptr,
                 COPYFILE_DATA | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW_SRC) != 0) {
      return result;
    }
    snapshot_executable = snapshot_root / relative;
  } else {
    snapshot_root = temp_root / source_root.filename();
    snapshot_executable = snapshot_root;
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
