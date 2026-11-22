#pragma once

#include "foundation/crypto/SignedBytes.h"

#include "common/ExtArea.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Every signed relay call's bytes (release-compatibility SIGNED_FORMATS § 2.8; docs/contracts/SERVICE_ENDPOINTS.md
 * § The audience), one layout for every domain at one sign version:
 *
 *   domain ‖ 0x00 ‖ u8 4 ‖ LenUtf8(audience) ‖ …the domain's fields… ‖ ext area
 *
 * The audience is the PeerId of the relay the call is for (its certificate's transport key, § 3.4; multi-relay MR007).
 * The body names the version as `sign_version` (read first by the relay: an older one answers 426
 * `client_upgrade_required`, a newer one 400 `relay_upgrade_required`), the extension area's entries as `ext_b64`
 * (left out when empty), and carries a typed signature (`SignRelayCommand`, lib/net/RelayAudience.h). No
 * `signature_alg`: the typed blobs name their algorithm.
 */
inline constexpr uint8_t kRelaySignVersion = 4;

inline constexpr char kRelayRegisterDomain[] = "pp-browser:relay-register";
inline constexpr char kRelayRevokeDomain[] = "pp-browser:relay-revoke";
inline constexpr char kRelayProfileDomain[] = "pp-browser:relay-profile";
inline constexpr char kRelayProfileIconDomain[] = "pp-browser:relay-profile-icon";
inline constexpr char kRelayHeartbeatDomain[] = "pp-browser:relay-heartbeat";
inline constexpr char kRelayLlmTokenDomain[] = "pp-browser:relay-llm-token";
inline constexpr char kRelayLockerPutDomain[] = "pp-browser:relay-locker-put";
inline constexpr char kRelayBlobPresignDomain[] = "pp-browser:relay-blob-presign";
inline constexpr char kRelayBlobRetainDomain[] = "pp-browser:relay-blob-retain";
inline constexpr char kRelayBlobDeleteDomain[] = "pp-browser:relay-blob-delete";
inline constexpr char kRelayFileStartDomain[] = "pp-browser:relay-file-start";
inline constexpr char kRelayFilePartDomain[] = "pp-browser:relay-file-part";
inline constexpr char kRelayFileStatusDomain[] = "pp-browser:relay-file-status";
inline constexpr char kRelayFileCompleteDomain[] = "pp-browser:relay-file-complete";
inline constexpr char kRelayFileDeleteDomain[] = "pp-browser:relay-file-delete";
inline constexpr char kRelayFileListDomain[] = "pp-browser:relay-file-list";
inline constexpr char kRelayApiDomain[] = "pp-browser:relay-api";

/** Every domain a relay call is signed in (the vector file, relay_sign_vectors.json, covers each). */
inline constexpr const char* kRelaySignDomains[] = {
    kRelayRegisterDomain,   kRelayRevokeDomain,       kRelayProfileDomain,    kRelayProfileIconDomain,
    kRelayHeartbeatDomain,  kRelayLlmTokenDomain,     kRelayLockerPutDomain,  kRelayBlobPresignDomain,
    kRelayBlobRetainDomain, kRelayBlobDeleteDomain,   kRelayFileStartDomain,  kRelayFilePartDomain,
    kRelayFileStatusDomain, kRelayFileCompleteDomain, kRelayFileDeleteDomain, kRelayFileListDomain,
    kRelayApiDomain,
};

/** Appends one call's fields after the header (the area is appended after them). */
using RelayCallFields = std::function<void(SignedBytesWriter&)>;

/**
 * One call's signed bytes: the header for `audience`, `fields`, then `ext`. Empty when `audience` is empty: no call is
 * signed for no relay.
 */
std::vector<uint8_t> BuildRelayCallBytes(std::string_view domain, const std::string& audience, const pp::ExtArea& ext,
                                         const RelayCallFields& fields);

/** The version byte of a signed relay call's bytes (the byte after the domain's NUL); -1 when there is none. */
int RelaySignVersionOf(const std::vector<uint8_t>& bytes);

/** `bytes` end with `ext`'s area form (`SignRelayCommand` checks the area it sends is the one signed). */
bool RelayCallBytesEndWith(const std::vector<uint8_t>& bytes, const pp::ExtArea& ext);

} // namespace pbr
