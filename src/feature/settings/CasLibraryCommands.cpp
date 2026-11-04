#include "feature/settings/CasLibraryCommands.h"

#include "lib/delivery/RelayClients.h"
#include "common/directory/DirectoryJson.h"
#include "common/thread/ChatBlobTypes.h"
#include "lib/content/CasLibrary.h"
#include "lib/net/OrgBackendClients.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

Roe<std::vector<CasLibraryItemView>> ListCasLibraryForSettings(const CasStore& content, std::string filter) {
  auto parsed = CasLibraryFilterFromString(filter);
  const CasLibraryFilter lib_filter = parsed.value_or(CasLibraryFilter::All);
  auto rows = ListCasLibrary(content, lib_filter);
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

Roe<void> ShareCasPubliclyForSettings(CasStore& content, const ByteVector& dek,
                                      const std::string& private_content_id_hex) {
  auto id = HexToBytes(private_content_id_hex);
  if (!id) {
    return id.error();
  }
  auto published = ShareCasPublicly(content, dek, *id);
  if (!published) {
    return published.error();
  }
  return {};
}

Roe<void> UnpublishCasForSettings(CasStore& content, const std::string& public_content_id_hex) {
  auto id = HexToBytes(public_content_id_hex);
  if (!id) {
    return id.error();
  }
  return UnpublishCasPublic(content, *id);
}

void FetchCasPublicTipForSettingsAsync(std::shared_ptr<CasStore> content, IChatBlobPeerClient& blob, const std::string& local_relay_user_id,
                                       const std::string& tip, const std::string& peer_relay_user_id,
                                       std::function<void(Roe<void>)> on_done) {
  if (!content) {
    on_done(Error("The content store is not open"));
    return;
  }
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
  request.requester_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.requester_identity_value = local_relay_user_id;
  request.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay_user_id;
  request.content_hash_hex = BytesToHex(*content_id);
  request.channel = ThreadChannel::E2e;
  blob.FetchChatBlobAsync(request, [content = std::move(content), content_id = *content_id,
                                    on_done = std::move(on_done)](Roe<std::vector<uint8_t>> bytes) {
    if (!bytes) {
      on_done(bytes.error());
      return;
    }
    // The fetch completes on the mesh IO strand: the cache write (disk) goes to a worker.
    AppRuntime::PostWorkerNormal([content, content_id, on_done, bytes = std::move(*bytes)]() {
      on_done(CacheFetchedPublicCas(*content, content_id, ByteVector(bytes.begin(), bytes.end())));
    });
  });
}

} // namespace pbr
