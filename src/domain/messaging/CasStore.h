#pragma once

#include "domain/messaging/CasTypes.h"
#include "domain/messaging/ObjectIndex.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>
#include <string_view>

namespace pbr {

/**
 * Profile-level content-addressed store with private/public realms ([content-cas] C001–C005, C010).
 *
 * Layout:
 *   {profile}/cas/private/blocks/{aa}/{bb}/{content_id_hex}  — PPBA + FileCipher under DEK
 *   {profile}/cas/public/blocks/{aa}/{bb}/{content_id_hex}   — clear published bytes
 *   {profile}/object_index.db
 *
 * Block paths use two hex prefix levels (256² leaf dirs) so large stores (e.g. pp-node)
 * do not flatten millions of files into one directory.
 */
class CasStore {
public:
  CasStore(std::string profile_dir, std::string profile_id);

  ObjectIndex& Index() { return index_; }
  const ObjectIndex& Index() const { return index_; }

  std::string BlocksRoot(CasRealm realm) const;
  std::string BlockPath(CasRealm realm, const ByteVector& content_id) const;

  bool Exists(CasRealm realm, const ByteVector& content_id) const;

  Roe<void> PutPrivate(const ByteVector& content_id, const ByteVector& plaintext, const ByteVector& dek,
                       std::string_view mime = {}, std::string_view filename = {}, bool pinned = true);

  Roe<ByteVector> GetPrivate(const ByteVector& content_id, const ByteVector& dek) const;

  Roe<void> PutPublic(const ByteVector& content_id, const ByteVector& published_bytes,
                      std::string_view mime = {}, std::string_view filename = {},
                      std::string_view published_from_hex = {}, bool pinned = true);

  Roe<ByteVector> GetPublic(const ByteVector& content_id) const;

  Roe<void> Delete(CasRealm realm, const ByteVector& content_id);

  /**
   * Share publicly… (C002 / C013): load private plaintext → put new public object (pinned),
   * with published_from_hex → private id. Returns the public content id.
   */
  Roe<ByteVector> PublishFromPrivate(const ByteVector& private_content_id, const ByteVector& dek);

  /** Unpublish thin slice (C006 / C013): unpin public row (stop-provide lands with P4). */
  Roe<void> Unpublish(const ByteVector& public_content_id);

private:
  Roe<void> WriteBlockFile(const std::string& path, const ByteVector& bytes) const;
  Roe<ByteVector> ReadBlockFile(const std::string& path) const;

  std::string profile_dir_;
  std::string profile_id_;
  ObjectIndex index_;
};

std::string BuildCasPrivateAad(std::string_view profile_id, std::string_view content_id_hex);

} // namespace pbr
