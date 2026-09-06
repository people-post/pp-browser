#include "domain/messaging/CasLibrary.h"

#include "domain/messaging/CasStore.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sstream>

namespace pbr {
namespace {

std::string FormatBytes(const uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  std::ostringstream out;
  out.setf(std::ios::fixed);
  if (bytes < static_cast<uint64_t>(kKiB)) {
    out << bytes << " B";
  } else if (bytes < static_cast<uint64_t>(kMiB)) {
    out.precision(bytes < static_cast<uint64_t>(10 * kKiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kKiB) << " KB";
  } else {
    out.precision(bytes < static_cast<uint64_t>(10 * kMiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kMiB) << " MB";
  }
  return out.str();
}

CasLibraryRow ToRow(const CasObjectMeta& meta) {
  CasLibraryRow row;
  row.content_id_hex = BytesToHex(meta.content_id);
  row.realm = meta.realm;
  row.title = meta.filename.empty() ? row.content_id_hex.substr(0, 12) + "…" : meta.filename;
  row.detail = FormatBytes(meta.byte_length);
  if (!meta.mime.empty()) {
    row.detail += " · ";
    row.detail += meta.mime;
  }
  row.realm_label = meta.realm == CasRealm::Public ? "Public" : "Private";
  row.pin_label = meta.pinned ? "Kept" : "Cache";
  row.pinned = meta.pinned;
  row.can_share_publicly = meta.realm == CasRealm::Private && meta.pinned;
  row.can_unpublish = meta.realm == CasRealm::Public && meta.pinned;
  row.can_copy_tip = meta.realm == CasRealm::Public && meta.pinned;
  return row;
}

} // namespace

Roe<std::vector<CasLibraryRow>> ListCasLibrary(const std::string& profile_dir, const CasLibraryFilter filter) {
  if (profile_dir.empty()) {
    return Error("CAS library requires profile directory");
  }
  ObjectIndex index(profile_dir);
  std::optional<CasRealm> realm;
  std::optional<bool> pinned;
  switch (filter) {
  case CasLibraryFilter::All:
    break;
  case CasLibraryFilter::Private:
    realm = CasRealm::Private;
    break;
  case CasLibraryFilter::Public:
    realm = CasRealm::Public;
    pinned = true;
    break;
  case CasLibraryFilter::Cache:
    pinned = false;
    break;
  }
  auto listed = index.List(realm, pinned);
  if (!listed) {
    return listed.error();
  }
  std::vector<CasLibraryRow> rows;
  rows.reserve(listed->size());
  for (const CasObjectMeta& meta : *listed) {
    rows.push_back(ToRow(meta));
  }
  return rows;
}

Roe<ByteVector> ShareCasPublicly(const std::string& profile_dir, const std::string& profile_id,
                                 const ByteVector& dek, const ByteVector& private_content_id) {
  CasStore store(profile_dir, profile_id);
  return store.PublishFromPrivate(private_content_id, dek);
}

Roe<void> UnpublishCasPublic(const std::string& profile_dir, const std::string& profile_id,
                             const ByteVector& public_content_id) {
  CasStore store(profile_dir, profile_id);
  return store.Unpublish(public_content_id);
}

std::string FormatCasPublicTip(const std::string& content_id_hex) {
  return std::string(kCasPublicTipPrefix) + content_id_hex;
}

Roe<ByteVector> ParseCasPublicTip(const std::string_view tip) {
  std::string hex(tip);
  while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) {
    hex.pop_back();
  }
  while (!hex.empty() && hex.front() == ' ') {
    hex.erase(hex.begin());
  }
  constexpr std::string_view kPrefix = kCasPublicTipPrefix;
  if (hex.size() >= kPrefix.size() && hex.compare(0, kPrefix.size(), kPrefix) == 0) {
    hex = hex.substr(kPrefix.size());
  }
  if (hex.size() != kCasContentIdSize * 2) {
    return Error("CAS tip must be pp-cas:v1:<64-hex> or a 64-hex content id");
  }
  auto id = HexToBytes(hex);
  if (!id) {
    return id.error();
  }
  if (id->size() != kCasContentIdSize) {
    return Error("Invalid CAS content id in tip");
  }
  return id;
}

Roe<ByteVector> LoadPinnedPublicCasBytes(const std::string& profile_dir, const std::string& profile_id,
                                         const ByteVector& content_id) {
  CasStore store(profile_dir, profile_id);
  auto meta = store.Index().Lookup(CasRealm::Public, content_id);
  if (!meta) {
    return meta.error();
  }
  if (!meta->has_value()) {
    return Error("Public CAS object not found");
  }
  if (!(*meta)->pinned) {
    return Error("Public CAS object is not Kept (provide refused)");
  }
  return store.GetPublic(content_id);
}

Roe<void> CacheFetchedPublicCas(const std::string& profile_dir, const std::string& profile_id,
                                const ByteVector& content_id, const ByteVector& bytes,
                                const std::string_view mime, const std::string_view filename) {
  CasStore store(profile_dir, profile_id);
  // Peer-fetched public → unpinned Cache (C013).
  return store.PutPublic(content_id, bytes, mime, filename, /*published_from_hex=*/{}, /*pinned=*/false);
}

} // namespace pbr
