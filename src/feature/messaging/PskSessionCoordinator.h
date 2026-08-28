#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/IThreadStore.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct PskExportView {
  std::string master_psk_b64;
  std::string fingerprint;
};

struct PskSessionStatus {
  bool has_psk = false;
  bool verified = false;
  std::string fingerprint;
  uint32_t session_epoch = 1;
};

/** E011/E020 — PSK generate, import, verify, and rotate orchestration. */
class PskSessionCoordinator {
public:
  PskSessionCoordinator(IThreadStore& store, IPskSessionStore& psk_store);

  Roe<PskSessionStatus> GetStatus(const std::string& thread_id) const;
  Roe<PskExportView> EnsureGenerated(const std::string& thread_id);
  Roe<PskExportView> GetExportView(const std::string& thread_id) const;
  Roe<std::string> ExportBundleJson(const std::string& thread_id) const;
  Roe<void> ImportRawBase64(const std::string& thread_id, const std::string& raw_b64);
  Roe<void> ImportBundleJson(const std::string& thread_id, const std::string& bundle_json);
  Roe<void> MarkVerified(const std::string& thread_id, int64_t verified_at_ms);
  Roe<std::string> RotatePskAndExportBundle(const std::string& thread_id, int64_t retired_at_ms);

private:
  Roe<ChatTargetKey> TargetKeyForThread(const std::string& thread_id) const;
  Roe<void> ApplyBundleToThread(const std::string& thread_id, const ChatTargetKey& key, const PskBundleV1& bundle);

  IThreadStore& store_;
  IPskSessionStore& psk_store_;
};

} // namespace pbr
