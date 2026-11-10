#include "foundation/platform/os/OsSecureBlob.h"

#include "foundation/platform/os/KeychainStatus.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
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

// The PIN grace key uses WhenUnlockedThisDeviceOnly: not readable while the phone is locked (#21 phase 1).
CFStringRef AccessLevel(const SecureBlobAccess access) {
  switch (access) {
    case SecureBlobAccess::kWhenUnlocked:
    case SecureBlobAccess::kBiometryCurrentSet: // written through WriteBiometryBound (an access control, not this)
      return kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
    case SecureBlobAccess::kAfterFirstUnlock:
      break;
  }
  return kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
}

OSStatus AddItem(const std::string& name, const UInt8* bytes, const CFIndex length, CFStringRef level) {
  NSMutableDictionary* query = BaseQuery(name);
  query[(id)kSecValueData] = [NSData dataWithBytes:bytes length:static_cast<NSUInteger>(length)];
  query[(id)kSecAttrAccessible] = (__bridge id)level;
  return SecItemAdd((__bridge CFDictionaryRef)query, nullptr);
}

// The biometric unlock key (at-rest A014): readable only after the enrolled face / finger passes the system prompt,
// this device only, and unreadable for good (errSecItemNotFound) once the enrolment changes. Always delete then add:
// SecItemAdd on a leftover item (a reinstall keeps Keychain items) answers errSecDuplicateItem, and SecItemUpdate or
// reading the old item would show the prompt. Neither the delete nor the add needs one; the add's UI is allowed
// (the default; kSecUseAuthenticationUIAllow is deprecated since iOS 14).
Roe<void> WriteBiometryBound(const std::string& name, const std::vector<uint8_t>& bytes) {
  CFErrorRef error = nullptr;
  SecAccessControlRef control = SecAccessControlCreateWithFlags(
      kCFAllocatorDefault, kSecAttrAccessibleWhenUnlockedThisDeviceOnly, kSecAccessControlBiometryCurrentSet, &error);
  if (control == nullptr) {
    const long code = error != nullptr ? static_cast<long>(CFErrorGetCode(error)) : 0;
    if (error != nullptr) {
      CFRelease(error);
    }
    return Error("SecAccessControlCreateWithFlags failed: " + std::to_string(code));
  }
  OSStatus status = SecItemDelete((__bridge CFDictionaryRef)BaseQuery(name));
  if (status != errSecSuccess && status != errSecItemNotFound) {
    CFRelease(control);
    return Error(StatusText("SecItemDelete", status));
  }
  NSMutableDictionary* query = BaseQuery(name);
  query[(id)kSecValueData] = [NSData dataWithBytes:bytes.data() length:bytes.size()];
  query[(id)kSecAttrAccessControl] = (__bridge id)control;
  status = SecItemAdd((__bridge CFDictionaryRef)query, nullptr);
  CFRelease(control);
  if (status != errSecSuccess) {
    return Error(StatusText("SecItemAdd", status));
  }
  return {};
}

// One read for both lookups. `context` (may be nil) carries the prompt's text for a biometry-bound item
// (LAContext.localizedReason, the replacement of the deprecated kSecUseOperationPrompt); a fresh context per read,
// so an earlier pass is never reused.
pbr::SecureBlobLookup LookupItem(const std::string& name, LAContext* context) {
  NSMutableDictionary* query = BaseQuery(name);
  query[(id)kSecReturnData] = @YES;
  query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
  if (context != nil) {
    query[(id)kSecUseAuthenticationContext] = context;
  }
  CFTypeRef result = nullptr;
  const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
  pbr::SecureBlobLookup out;
  switch (MapKeychainStatus(static_cast<int32_t>(status))) {
    case KeychainLookup::Found:
      if (result != nullptr && CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef data = static_cast<CFDataRef>(result);
        const UInt8* begin = CFDataGetBytePtr(data);
        out.bytes.assign(begin, begin + CFDataGetLength(data));
        out.status = pbr::SecureBlobLookup::Status::Found;
      } else {
        out.status = pbr::SecureBlobLookup::Status::Unavailable; // success without data: unexpected, keep it
        out.code = status;
      }
      break;
    case KeychainLookup::Missing:
      out.status = pbr::SecureBlobLookup::Status::Missing;
      break;
    case KeychainLookup::Unavailable:
      out.status = pbr::SecureBlobLookup::Status::Unavailable;
      out.code = status;
      break;
    case KeychainLookup::Canceled:
      out.status = pbr::SecureBlobLookup::Status::Canceled;
      out.code = status;
      break;
  }
  if (result != nullptr) {
    CFRelease(result);
  }
  return out;
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

Roe<void> SecureBlobWriteWithAccess(const std::string& name, const std::vector<uint8_t>& bytes,
                                    const SecureBlobAccess access) {
  @autoreleasepool {
    if (access == SecureBlobAccess::kBiometryCurrentSet) {
      return WriteBiometryBound(name, bytes);
    }
    CFStringRef level = AccessLevel(access);
    NSMutableDictionary* query = BaseQuery(name);
    query[(id)kSecReturnAttributes] = @YES;
    query[(id)kSecReturnData] = @YES;
    query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status == errSecItemNotFound) {
      status = AddItem(name, bytes.data(), static_cast<CFIndex>(bytes.size()), level);
      if (status != errSecSuccess) {
        return Error(StatusText("SecItemAdd", status));
      }
      return {};
    }
    if (status != errSecSuccess || result == nullptr || CFGetTypeID(result) != CFDictionaryGetTypeID()) {
      if (result != nullptr) {
        CFRelease(result);
      }
      return Error(StatusText("SecItemCopyMatching", status));
    }
    CFDictionaryRef found = static_cast<CFDictionaryRef>(result);
    const void* old_level = CFDictionaryGetValue(found, kSecAttrAccessible);
    if (old_level != nullptr && CFEqual(old_level, level)) {
      CFRelease(result);
      NSDictionary* update = @{(id)kSecValueData : [NSData dataWithBytes:bytes.data() length:bytes.size()]};
      status = SecItemUpdate((__bridge CFDictionaryRef)BaseQuery(name), (__bridge CFDictionaryRef)update);
      if (status != errSecSuccess) {
        return Error(StatusText("SecItemUpdate", status));
      }
      return {};
    }
    // Another level: an item's accessibility cannot be changed in place under the same name, so delete and add;
    // if the add fails, put the old bytes back under the old level (best effort) and report the failure.
    status = SecItemDelete((__bridge CFDictionaryRef)BaseQuery(name));
    if (status != errSecSuccess && status != errSecItemNotFound) {
      CFRelease(result);
      return Error(StatusText("SecItemDelete", status));
    }
    status = AddItem(name, bytes.data(), static_cast<CFIndex>(bytes.size()), level);
    if (status != errSecSuccess) {
      const void* old_data = CFDictionaryGetValue(found, kSecValueData);
      if (old_data != nullptr && old_level != nullptr && CFGetTypeID(old_data) == CFDataGetTypeID() &&
          CFGetTypeID(old_level) == CFStringGetTypeID()) {
        CFDataRef old_bytes = static_cast<CFDataRef>(old_data);
        (void)AddItem(name, CFDataGetBytePtr(old_bytes), CFDataGetLength(old_bytes),
                      static_cast<CFStringRef>(old_level));
      }
      CFRelease(result);
      return Error(StatusText("SecItemAdd", status));
    }
    CFRelease(result);
    return {};
  }
}

Roe<pbr::SecureBlobLookup> SecureBlobLookup(const std::string& name) {
  @autoreleasepool {
    return LookupItem(name, nil);
  }
}

Roe<pbr::SecureBlobLookup> SecureBlobLookupWithPrompt(const std::string& name, const std::string& prompt) {
  @autoreleasepool {
    LAContext* context = [[LAContext alloc] init];
    NSString* reason = [NSString stringWithUTF8String:prompt.c_str()];
    if (reason != nil && reason.length > 0) {
      context.localizedReason = reason;
    }
    pbr::SecureBlobLookup out = LookupItem(name, context);
    [context release];
    return out;
  }
}

bool SecureBlobSupported() {
  return true;
}

} // namespace pbr::os
