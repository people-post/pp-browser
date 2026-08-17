#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

inline constexpr uint8_t kAadVersion = 1;
inline constexpr uint8_t kEncryptedPayloadVersion = 1;

inline constexpr size_t kMasterPskSize = 32;
inline constexpr size_t kSessionKeySize = 32;
inline constexpr size_t kDataEncryptionKeySize = 32;
inline constexpr size_t kAeadNonceSize = 24;
inline constexpr size_t kPskFingerprintSize = 32;

inline constexpr uint8_t kVaultFileVersion = 1;

inline constexpr const char* kHkdfSalt = "pp-browser-msg-v1";
inline constexpr const char* kAutoKeyHkdfInfoPrefix = "auto-key-v1|channel:";

inline constexpr size_t kReplayWindowSize = 32;
inline constexpr uint32_t kMaxRetiredPskEpochs = 8;

inline constexpr size_t kMaxE2ePlaintextBytes = 128 * 1024;
inline constexpr size_t kMaxPskBundleBytes = 4 * 1024;
/** Quiet auto-`rotate_psk` on public `device_pair` (E027). */
inline constexpr uint32_t kPublicPskAutoRotateMsgCount = 100;
inline constexpr int64_t kPublicPskAutoRotateIntervalMs = 7LL * 24 * 60 * 60 * 1000;
/** ML-DSA-65 secret is ~4 KiB; leave room for public PSKs (M012). */
inline constexpr size_t kMaxLinkDeviceBundleBytes = 96 * 1024;
inline constexpr int64_t kLinkDeviceDefaultTtlMs = 15 * 60 * 1000;

} // namespace pbr
