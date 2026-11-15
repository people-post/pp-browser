#include "gui/SettingsMyRelays.h"

#include "feature/settings/MyRelays.h"
#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>

#include <utility>

namespace pbr {

SettingsMyRelays::SettingsMyRelays(const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                                   std::function<void(const Error&)> report_failure)
    : commands_(commands), feedback_(feedback), report_failure_(std::move(report_failure)) {}

SettingsMyRelays::~SettingsMyRelays() {
  *alive_ = false;
  if (watching_ && commands_.watch_my_relays) {
    commands_.watch_my_relays(nullptr);
  }
}

void SettingsMyRelays::Bind(ui::DataModelConstructor& ctor) {
  if (auto row = ctor.RegisterStruct<Row>()) {
    row.RegisterMember("origin", &Row::origin);
    row.RegisterMember("label", &Row::label);
    row.RegisterMember("state", &Row::state);
    row.RegisterMember("state_kind", &Row::state_kind);
    row.RegisterMember("reach", &Row::reach);
    row.RegisterMember("reach_failed", &Row::reach_failed);
    row.RegisterMember("preferred", &Row::preferred);
    row.RegisterMember("can_make_preferred", &Row::can_make_preferred);
    row.RegisterMember("can_remove", &Row::can_remove);
    row.RegisterMember("can_accept_identity", &Row::can_accept_identity);
  }
  ctor.RegisterArray<std::vector<Row>>();
  ctor.Bind("my_relay_rows", &rows_);
  ctor.Bind("my_relays_shown", &shown_);
  ctor.Bind("my_relays_editable", &editable_);
  ctor.Bind("my_relays_can_add", &can_add_);
  ctor.Bind("my_relay_adding", &adding_);
  ctor.Bind("my_relays_busy", &busy_);
  ctor.Bind("my_relay_draft", &draft_);
  ctor.Bind("my_relay_add_error", &add_error_);
  ctor.Bind("my_relays_org_changed", &org_identity_changed_);
  ctor.BindEventCallback("start_add_my_relay", DataEvent<>([this]() { StartAdd(); }));
  ctor.BindEventCallback("cancel_add_my_relay", DataEvent<>([this]() { CancelAdd(); }));
  ctor.BindEventCallback("add_my_relay", DataEvent<>([this]() { Add(); }));
  ctor.BindEventCallback("make_relay_preferred", DataEvent<int>([this](const int index) { MakePreferred(index); }));
  ctor.BindEventCallback("remove_my_relay", DataEvent<int>([this](const int index) { Remove(index); }));
  ctor.BindEventCallback("accept_relay_identity", DataEvent<int>([this](const int index) { AcceptIdentity(index); }));
  ctor.BindEventCallback("accept_org_identity", DataEvent<>([this]() { AcceptOrgIdentity(); }));
}

void SettingsMyRelays::Watch() {
  if (!commands_.watch_my_relays) {
    return; // set again at every read: the ports may have been bound anew (a profile reset)
  }
  watching_ = true;
  std::weak_ptr<bool> alive = alive_;
  commands_.watch_my_relays([this, alive]() {
    AppRuntime::PostUI([this, alive]() {
      if (auto live = alive.lock(); live && *live) {
        Refresh();
      }
    });
  });
}

void SettingsMyRelays::Refresh() {
  if (!commands_.my_relays) {
    Show(Error("Messaging is not ready"));
    return;
  }
  Watch();
  const uint64_t count = ++refresh_count_;
  std::weak_ptr<bool> alive = alive_;
  AppRuntime::PostWorkerNormal([this, count, alive]() {
    auto listed = commands_.my_relays ? commands_.my_relays() : Roe<MyRelaysView>(Error("Messaging is not ready"));
    AppRuntime::PostUI([this, count, alive, listed = std::move(listed)]() mutable {
      if (auto live = alive.lock(); live && *live && count == refresh_count_) {
        Show(std::move(listed));
      }
    });
  });
}

void SettingsMyRelays::Show(Roe<MyRelaysView> listed) {
  // Messaging not up (or no account yet): nothing to show, and nothing to report.
  view_ = listed ? std::move(*listed) : MyRelaysView{};
  rows_.clear();
  rows_.reserve(view_.relays.size());
  for (const MyRelayRowView& relay : view_.relays) {
    rows_.push_back({.origin = relay.origin.c_str(),
                     .label = relay.label.c_str(),
                     .state = relay.state.c_str(),
                     .state_kind = relay.state_kind.c_str(),
                     .reach = relay.reach.c_str(),
                     .reach_failed = relay.reach_failed,
                     .preferred = relay.preferred,
                     .can_make_preferred = relay.can_make_preferred,
                     .can_remove = relay.can_remove,
                     .can_accept_identity = relay.can_accept_identity});
  }
  org_identity_changed_ = MyRelayLabel(view_.org_identity_changed).c_str();
  shown_ = !rows_.empty();
  editable_ = view_.editable;
  can_add_ = view_.can_add;
  if (!can_add_) {
    adding_ = false;
  }
  DirtyAll();
}

void SettingsMyRelays::DirtyAll() {
  DataModelHost& host = DataModelHost::Instance();
  for (const char* field :
       {"my_relay_rows", "my_relays_shown", "my_relays_editable", "my_relays_can_add", "my_relay_adding",
        "my_relays_busy", "my_relay_draft", "my_relay_add_error", "my_relays_org_changed"}) {
    host.Dirty("settings", field);
  }
}

void SettingsMyRelays::StartAdd() {
  adding_ = can_add_;
  draft_ = "";
  add_error_ = "";
  DirtyAll();
}

void SettingsMyRelays::CancelAdd() {
  adding_ = false;
  add_error_ = "";
  DirtyAll();
}

void SettingsMyRelays::Add() {
  if (busy_ || !adding_) {
    return;
  }
  if (!commands_.add_my_relay) {
    report_failure_(Error("Messaging is not ready"));
    return;
  }
  AddNow(draft_.c_str(), false);
}

void SettingsMyRelays::AddNow(const std::string& typed, const bool accept_changed) {
  busy_ = true;
  add_error_ = "";
  DirtyAll();
  std::weak_ptr<bool> alive = alive_;
  AppRuntime::PostWorkerNormal([this, typed, accept_changed, alive, add = commands_.add_my_relay]() {
    Roe<void> added = add(typed, accept_changed);
    AppRuntime::PostUI([this, alive, typed, added = std::move(added)]() {
      if (auto live = alive.lock(); !live || !*live) {
        return;
      }
      busy_ = false;
      if (!added && added.error().message == "relay_identity_changed_since_seen") {
        DirtyAll();
        ConfirmChangedSinceSeen(typed); // MR007: a relay we saw with another identity, added only on confirm
        return;
      }
      if (!added) {
        add_error_ = MyRelaysFailureText(added.error()).c_str(); // under the field: the user fixes it there
        DirtyAll();
        return;
      }
      adding_ = false;
      add_error_ = "";
      draft_ = "";
      Refresh();
      UserFeedback::Ok(Tr("settings.relays.added"));
    });
  });
}

void SettingsMyRelays::ConfirmChangedSinceSeen(const std::string& typed) {
  if (!feedback_.show_confirm) {
    add_error_ = Tr("settings.relays.add.identity_changed_message").c_str();
    DirtyAll();
    return;
  }
  feedback_.show_confirm(
      Tr("settings.relays.add.identity_changed_title"), Tr("settings.relays.add.identity_changed_message"),
      [this, typed](const bool ok) {
        if (ok && !busy_) {
          AddNow(typed, true);
        }
      },
      Tr("settings.relays.add.identity_changed_ok"));
}

const MyRelayRowView* SettingsMyRelays::RowAt(const int index) const {
  if (index < 0 || index >= static_cast<int>(view_.relays.size())) {
    return nullptr;
  }
  return &view_.relays[static_cast<size_t>(index)];
}

void SettingsMyRelays::MakePreferred(const int index) {
  const MyRelayRowView* row = RowAt(index);
  if (!row || !row->can_make_preferred) {
    return;
  }
  Change(row->origin, commands_.make_relay_preferred, Tr("settings.relays.made_preferred"));
}

void SettingsMyRelays::Remove(const int index) {
  const MyRelayRowView* row = RowAt(index);
  if (!row || !row->can_remove || !feedback_.show_confirm) {
    return;
  }
  const auto& show_confirm =
      feedback_.show_confirm_destructive ? feedback_.show_confirm_destructive : feedback_.show_confirm;
  show_confirm(
      Tr("settings.relays.remove_confirm_title"), Tr("settings.relays.remove_confirm_message", {{"relay", row->label}}),
      // The origin, not the row: the list may be read again (and reordered) while the dialog is open.
      [this, origin = row->origin](const bool ok) {
        if (ok) {
          Change(origin, commands_.remove_my_relay, Tr("settings.relays.removed"));
        }
      },
      Tr("settings.relays.remove_confirm_ok"));
}

void SettingsMyRelays::AcceptIdentity(const int index) {
  const MyRelayRowView* row = RowAt(index);
  if (row && row->can_accept_identity) {
    ConfirmAccept(row->origin, row->label);
  }
}

void SettingsMyRelays::AcceptOrgIdentity() {
  if (!view_.org_identity_changed.empty()) {
    ConfirmAccept(view_.org_identity_changed, MyRelayLabel(view_.org_identity_changed));
  }
}

void SettingsMyRelays::ConfirmAccept(const std::string& origin, const std::string& label) {
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(
      Tr("settings.relays.accept_confirm_title"), Tr("settings.relays.accept_confirm_message", {{"relay", label}}),
      // The origin, not the row: the list may be read again while the dialog is open.
      [this, origin](const bool ok) {
        if (ok) {
          Change(origin, commands_.accept_relay_identity, Tr("settings.relays.accepted"));
        }
      },
      Tr("settings.relays.accept_confirm_ok"));
}

void SettingsMyRelays::Change(const std::string& origin, std::function<Roe<void>(const std::string& origin)> change,
                              const std::string& done) {
  if (origin.empty() || busy_) {
    return;
  }
  if (!change) {
    report_failure_(Error("Messaging is not ready"));
    return;
  }
  busy_ = true;
  DirtyAll();
  std::weak_ptr<bool> alive = alive_;
  AppRuntime::PostWorkerNormal([this, alive, origin, change = std::move(change), done]() {
    Roe<void> changed = change(origin);
    AppRuntime::PostUI([this, alive, changed = std::move(changed), done]() {
      if (auto live = alive.lock(); !live || !*live) {
        return;
      }
      busy_ = false;
      Refresh();
      if (!changed) {
        report_failure_(Error(MyRelaysFailureText(changed.error())));
        return;
      }
      UserFeedback::Ok(done);
    });
  });
}

} // namespace pbr
