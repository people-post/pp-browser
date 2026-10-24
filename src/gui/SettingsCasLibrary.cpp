#include "gui/SettingsCasLibrary.h"

#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/content/CasLibrary.h"
#include "lib/ui/DataEvent.h"
#include "common/Utilities.h"

#include <ui/base/SystemInterface.h>
#include <ui/data/DataModelHandle.h>

#include <utility>

namespace pbr {

SettingsCasLibrary::SettingsCasLibrary(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                                       const UnlockEnsurePorts& unlock,
                                       std::function<void(const Error&)> report_failure)
    : commands_(commands), feedback_(feedback), unlock_(unlock), report_failure_(std::move(report_failure)) {}

void SettingsCasLibrary::Bind(ui::DataModelConstructor& ctor) {
  if (auto row = ctor.RegisterStruct<Row>()) {
    row.RegisterMember("content_id_hex", &Row::content_id_hex);
    row.RegisterMember("title", &Row::title);
    row.RegisterMember("detail", &Row::detail);
    row.RegisterMember("realm_label", &Row::realm_label);
    row.RegisterMember("pin_label", &Row::pin_label);
    row.RegisterMember("can_share_publicly", &Row::can_share_publicly);
    row.RegisterMember("can_unpublish", &Row::can_unpublish);
    row.RegisterMember("can_copy_tip", &Row::can_copy_tip);
  }
  ctor.RegisterArray<std::vector<Row>>();
  ctor.Bind("cas_library_rows", &rows_);
  ctor.Bind("cas_library_empty_label", &empty_label_);
  ctor.BindEventCallback("set_cas_library_filter",
                         DataEvent<std::string>([this](const std::string& filter) { SetFilter(filter); }));
  ctor.BindEventCallback("share_cas_publicly", DataEvent<int>([this](int index) { ShareCasPublicly(index); }));
  ctor.BindEventCallback("unpublish_cas", DataEvent<int>([this](int index) { Unpublish(index); }));
  ctor.BindEventCallback("copy_cas_tip", DataEvent<int>([this](int index) { CopyTip(index); }));
  ctor.BindEventCallback("fetch_cas_tip", DataEvent<>([this]() { FetchTip(); }));
}

void SettingsCasLibrary::Refresh() {
  List();
  Show();
}

void SettingsCasLibrary::List() {
  items_.clear();
  empty_label_ = Tr("settings.storage.library.empty").c_str();
  if (!commands_.list_cas_library) {
    return;
  }
  auto listed = commands_.list_cas_library(filter_);
  if (!listed) {
    report_failure_(listed.error());
    return;
  }
  items_ = std::move(*listed);
}

void SettingsCasLibrary::Show() {
  rows_.clear();
  rows_.reserve(items_.size());
  for (const CasLibraryItemView& item : items_) {
    rows_.push_back({.content_id_hex = item.content_id_hex.c_str(),
                     .title = item.title.c_str(),
                     .detail = item.detail.c_str(),
                     .realm_label = item.realm_label.c_str(),
                     .pin_label = item.pin_label.c_str(),
                     .can_share_publicly = item.can_share_publicly,
                     .can_unpublish = item.can_unpublish,
                     .can_copy_tip = item.can_copy_tip});
  }
  DataModelHost::Instance().Dirty("settings", "cas_library_rows");
  DataModelHost::Instance().Dirty("settings", "cas_library_empty_label");
}

const CasLibraryItemView* SettingsCasLibrary::ItemAt(const int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    return nullptr;
  }
  return &items_[static_cast<size_t>(index)];
}

void SettingsCasLibrary::SetFilter(const std::string& filter) {
  filter_ = filter.empty() ? "all" : filter;
  Refresh();
}

void SettingsCasLibrary::ShareCasPublicly(const int index) {
  const CasLibraryItemView* item = ItemAt(index);
  if (!item || !feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(Tr("settings.storage.library.share_confirm_title"),
                         Tr("settings.storage.library.share_confirm_message"),
                         [this, content_id_hex = item->content_id_hex](const bool ok) {
                           if (!ok) {
                             return;
                           }
                           if (!unlock_.ensure_unlocked) {
                             PerformShare(content_id_hex);
                             return;
                           }
                           unlock_.ensure_unlocked([this, content_id_hex](const bool unlocked) {
                             if (unlocked) {
                               PerformShare(content_id_hex);
                             }
                           });
                         },
                         {});
}

void SettingsCasLibrary::Unpublish(const int index) {
  const CasLibraryItemView* item = ItemAt(index);
  if (!item || !feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(Tr("settings.storage.library.unpublish_confirm_title"),
                         Tr("settings.storage.library.unpublish_confirm_message"),
                         [this, content_id_hex = item->content_id_hex](const bool ok) {
                           if (ok) {
                             PerformUnpublish(content_id_hex);
                           }
                         },
                         {});
}

void SettingsCasLibrary::PerformShare(const std::string& content_id_hex) {
  if (!commands_.share_cas_publicly) {
    report_failure_(AppError::Storage(Err::Storage::Unavailable, "CAS share is not available"));
    return;
  }
  if (auto shared = commands_.share_cas_publicly(content_id_hex); !shared) {
    report_failure_(shared.error());
    return;
  }
  Refresh();
  UserFeedback::Ok(Tr("settings.storage.library.share_done"));
}

void SettingsCasLibrary::PerformUnpublish(const std::string& content_id_hex) {
  if (!commands_.unpublish_cas) {
    report_failure_(AppError::Storage(Err::Storage::Unavailable, "CAS unpublish is not available"));
    return;
  }
  if (auto unpublished = commands_.unpublish_cas(content_id_hex); !unpublished) {
    report_failure_(unpublished.error());
    return;
  }
  Refresh();
  UserFeedback::Ok(Tr("settings.storage.library.unpublish_done"));
}

void SettingsCasLibrary::CopyTip(const int index) {
  const CasLibraryItemView* item = ItemAt(index);
  if (!item || !item->can_copy_tip) {
    return;
  }
  const std::string tip = FormatCasPublicTip(item->content_id_hex);
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(tip.c_str());
  }
  UserFeedback::Ok(Tr("settings.storage.library.copy_tip_done"));
}

void SettingsCasLibrary::FetchTip() {
  if (!feedback_.show_prompt) {
    return;
  }
  feedback_.show_prompt(
      Tr("settings.storage.library.fetch_tip_title"), Tr("settings.storage.library.fetch_tip_message"), "",
      [this](const bool confirmed, const std::string& entered_tip) {
        const std::string tip = util::Trim(entered_tip);
        if (!confirmed || tip.empty()) {
          return;
        }
        feedback_.show_prompt(Tr("settings.storage.library.fetch_tip_peer_title"),
                              Tr("settings.storage.library.fetch_tip_peer_message"), "",
                              [this, tip](const bool peer_ok, const std::string& entered_peer) {
                                const std::string peer = util::Trim(entered_peer);
                                if (peer_ok && !peer.empty()) {
                                  PerformFetchTip(tip, peer);
                                }
                              });
      });
}

void SettingsCasLibrary::PerformFetchTip(const std::string& tip, const std::string& peer_relay_user_id) {
  if (!commands_.fetch_cas_public_tip) {
    report_failure_(AppError::Storage(Err::Storage::Unavailable, "CAS tip fetch is not available"));
    return;
  }
  // Answered on the UI thread once the mesh fetch lands (the library outlives the request).
  commands_.fetch_cas_public_tip(tip, peer_relay_user_id, [this](Roe<void> fetched) {
    if (!fetched) {
      report_failure_(fetched.error());
      return;
    }
    Refresh();
    UserFeedback::Ok(Tr("settings.storage.library.fetch_tip_done"));
  });
}

} // namespace pbr
