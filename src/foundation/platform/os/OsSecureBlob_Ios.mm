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

// The platform target compiles Objective-C++ without ARC: plain toll-free casts, manual CFRelease.
NSMutableDictionary* BaseQuery(const std::string& name) {
  NSMutableDictionary* query = [NSMutableDictionary dictionary];
  query[(id)kSecClass] = (id)kSecClassGenericPassword;
  query[(id)kSecAttrService] = [NSString stringWithUTF8String:kService];
  query[(id)kSecAttrAccount] = [NSString stringWithUTF8String:name.c_str()];
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
    NSDictionary* update = @{(id)kSecValueData : data};
    OSStatus status = SecItemUpdate((CFDictionaryRef)query, (CFDictionaryRef)update);
    if (status == errSecItemNotFound) {
      query[(id)kSecValueData] = data;
      query[(id)kSecAttrAccessible] = (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
      status = SecItemAdd((CFDictionaryRef)query, nullptr);
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
    query[(id)kSecReturnData] = @YES;
    query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching((CFDictionaryRef)query, &result);
    if (status == errSecItemNotFound) {
      return std::optional<std::vector<uint8_t>>{};
    }
    if (status != errSecSuccess || result == nullptr) {
      if (result != nullptr) {
        CFRelease(result);
      }
      return Error(StatusText("SecItemCopyMatching", status));
    }
    CFDataRef data = static_cast<CFDataRef>(result);
    const UInt8* begin = CFDataGetBytePtr(data);
    std::vector<uint8_t> bytes(begin, begin + CFDataGetLength(data));
    CFRelease(result);
    return std::optional<std::vector<uint8_t>>(std::move(bytes));
  }
}

Roe<void> SecureBlobDelete(const std::string& name) {
  @autoreleasepool {
    const OSStatus status = SecItemDelete((CFDictionaryRef)BaseQuery(name));
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
