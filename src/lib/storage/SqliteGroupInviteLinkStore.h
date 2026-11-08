#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "lib/messaging/IGroupInviteLinkStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * IGroupInviteLinkStore on profile.db (`group_invite_links`, G021): each link's secret encrypted with the profile DEK
 * (AAD purpose `group_link_secret`), the rest plain.
 */
class SqliteGroupInviteLinkStore : public Module, public IGroupInviteLinkStore, public IDekConsumer {
public:
  explicit SqliteGroupInviteLinkStore(std::string profile_db_path, std::string profile_id = {});

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  Roe<void> Keep(const KeptInviteLink& link) const override;
  Roe<std::optional<KeptInviteLink>> Find(const std::string& link_id) const override;
  Roe<std::vector<KeptInviteLink>> ListForGroup(const std::string& group_id) const override;
  Roe<void> Forget(const std::string& link_id) const override;
  Roe<void> ForgetExpiredBefore(int64_t before_ms) const override;

private:
  Roe<sqlite3*> OpenDb() const;
  /** The rows `sql` selects (its one text parameter bound), secrets decrypted when the DEK is set. */
  Roe<std::vector<KeptInviteLink>> Select(const char* sql, const std::string& param) const;
  Roe<void> Run(const char* sql, const std::string& text_param, std::optional<int64_t> int_param) const;

  std::string profile_db_path_;
  std::string profile_id_;
  mutable std::mutex schema_mutex_;
  mutable bool schema_ready_ = false;
  mutable std::mutex mutex_;
  ByteVector dek_;
};

} // namespace pbr
