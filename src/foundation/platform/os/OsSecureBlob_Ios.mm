#include "foundation/platform/os/OsSecureBlob.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <string>
#include <vector>

namespace pbr::os {

namespace {

// One generic-password item per name under this service. AfterFirstUnlockThisDeviceOnly: readable
// once the phone has been unlocked since boot (background relaunch after a reinstall still works),
// never synced to iCloud Keychain, never in device backups, gone with the device.
constexpr const char kService[] = "dev.pp-browser.secure-blob";

NSMutableDictionary* BaseQuery(const std::string& name) {
  NSMutableDictionary* query = [NSMutableDictionary dictionary];
  query[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
  query[(__bridge id)kSecAttrService] = [NSString stringWithUTF8String:kService];
  query[(__bridge id)kSecAttrAccount] = [NSString stringWithUTF8String:name.c_str()];
  return query;
}

std::string StatusText(const char* what, const OSStatus status) {
  return std::string(what) + " failed: OSStatus " + std::to_string(static_cast<long>(status));
}

} // namespace

Roe<void> SecureBlobWrite(const std::string& name, const std::vector<uint8_t>& bytes) {
  @autoreleasepool {
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    NSMutableDictionary* query = BaseQuery(name);
    NSDictionary* update = @{(__bridge id)kSecValueData : data};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)update);
    if (status == errSecItemNotFound) {
      query[(__bridge id)kSecValueData] = data;
      query[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
      status = SecItemAdd((__bridge CFDictionaryRef)query, nullptr);
    }
    if (status != errSecSuccess) {
      return Error(StatusText("SecItemAdd/Update", status));
    }
    return {};
  }
}

Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& name) {
  @autoreleasepool {
    NSMutableDictionary* query = BaseQuery(name);
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status == errSecItemNotFound) {
      return std::optional<std::vector<uint8_t>>{};
    }
    if (status != errSecSuccess || result == nullptr) {
      return Error(StatusText("SecItemCopyMatching", status));
    }
    NSData* data = (__bridge_transfer NSData*)result;
    const auto* begin = static_cast<const uint8_t*>(data.bytes);
    return std::optional<std::vector<uint8_t>>(std::vector<uint8_t>(begin, begin + data.length));
  }
}

Roe<void> SecureBlobDelete(const std::string& name) {
  @autoreleasepool {
    const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)BaseQuery(name));
    if (status != errSecSuccess && status != errSecItemNotFound) {
      return Error(StatusText("SecItemDelete", status));
    }
    return {};
  }
}

bool SecureBlobSupported() {
  return true;
}

} // namespace pbr::os
