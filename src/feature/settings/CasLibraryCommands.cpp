#include "feature/settings/CasLibraryCommands.h"

#include "common/directory/DirectoryJson.h"
#include "common/thread/ChatBlobTypes.h"
#include "domain/messaging/CasLibrary.h"
#include "domain/net/OrgBackendClients.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/runtime/AppRuntime.h"

namespace pbr {

Roe<std::vector<CasLibraryItemView>> ListCasLibraryForSettings(const std::string& profile_dir,
                                                               std::string filter) {
  auto parsed = CasLibraryFilterFromString(filter);
  const CasLibraryFilter lib_filter = parsed.value_or(CasLibraryFilter::All);
  auto rows = ListCasLibrary(profile_dir, lib_filter);
  if (!rows) {
    return rows.error();
  }
  std::vector<CasLibraryItemView> out;
  out.reserve(rows->size());
  for (const CasLibraryRow& row : *rows) {
    out.push_back({.content_id_hex = row.content_id_hex,
                   .title = row.title,
                   .detail = row.detail,
                   .realm_label = row.realm_label,
                   .pin_label = row.pin_label,
                   .can_share_publicly = row.can_share_publicly,
                   .can_unpublish = row.can_unpublish,
                   .can_copy_tip = row.can_copy_tip});
  }
  return out;
}

Roe<void> ShareCasPubliclyForSettings(const std::string& profile_dir, const std::string& profile_id,
                                      const ByteVector& dek, const std::string& private_content_id_hex) {
  auto id = HexToBytes(private_content_id_hex);
  if (!id) {
    return id.error();
  }
  auto published = ShareCasPublicly(profile_dir, profile_id, dek, *id);
  if (!published) {
    return published.error();
  }
  return {};
}

Roe<void> UnpublishCasForSettings(const std::string& profile_dir, const std::string& profile_id,
                                  const std::string& public_content_id_hex) {
  auto id = HexToBytes(public_content_id_hex);
  if (!id) {
    return id.error();
  }
  return UnpublishCasPublic(profile_dir, profile_id, *id);
}

void FetchCasPublicTipForSettingsAsync(const std::string& profile_dir, const std::string& profile_id,
                                       IChatBlobPeerClient& blob, const std::string& local_relay_user_id,
                                       const std::string& tip, const std::string& peer_relay_user_id,
                                       std::function<void(Roe<void>)> on_done) {
  if (!blob.IsPeerReachable(peer_relay_user_id)) {
    on_done(Error("Peer is not reachable for CAS tip fetch"));
    return;
  }
  auto content_id = ParseCasPublicTip(tip);
  if (!content_id) {
    on_done(content_id.error());
    return;
  }
  if (local_relay_user_id.empty()) {
    on_done(Error("Local relay identity missing"));
    return;
  }
  ChatBlobRequest request;
  request.op = ChatBlobOp::FetchPublic;
  request.requester_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.requester_identity_value = local_relay_user_id;
  request.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay_user_id;
  request.content_hash_hex = BytesToHex(*content_id);
  request.channel = ThreadChannel::E2e;
  blob.FetchChatBlobAsync(request, [profile_dir, profile_id, content_id = *content_id,
                                    on_done = std::move(on_done)](Roe<std::vector<uint8_t>> bytes) {
    if (!bytes) {
      on_done(bytes.error());
      return;
    }
    // The fetch completes on the mesh IO strand: the cache write (disk) goes to a worker.
    AppRuntime::PostWorkerNormal([profile_dir, profile_id, content_id, on_done,
                                  bytes = std::move(*bytes)]() {
      on_done(CacheFetchedPublicCas(profile_dir, profile_id, content_id, ByteVector(bytes.begin(), bytes.end())));
    });
  });
}

} // namespace pbr
