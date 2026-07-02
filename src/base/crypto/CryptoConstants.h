#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

inline constexpr uint8_t kAadVersion = 1;
inline constexpr uint8_t kEncryptedPayloadVersion = 1;

inline constexpr size_t kMasterPskSize = 32;
inline constexpr size_t kSessionKeySize = 32;
inline constexpr size_t kAeadNonceSize = 24;
inline constexpr size_t kPskFingerprintSize = 32;

inline constexpr const char* kHkdfSalt = "pp-browser-msg-v1";
inline constexpr const char* kAutoKeyHkdfInfoPrefix = "auto-key-v1|channel:";

inline constexpr size_t kReplayWindowSize = 32;
inline constexpr uint32_t kMaxRetiredPskEpochs = 8;

inline constexpr size_t kMaxE2ePlaintextBytes = 128 * 1024;
inline constexpr size_t kMaxPskBundleBytes = 4 * 1024;

} // namespace pbr
