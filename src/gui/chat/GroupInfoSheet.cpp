#include "gui/chat/GroupInfoSheet.h"

#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>

namespace pbr {

namespace {

constexpr const char* kModel = "group_info";
/** The group's rows change underneath the sheet (a member joins, a name changes): read again this often. */
constexpr auto kRereadEvery = std::chrono::seconds(1);

} // namespace

ChatHeaderSheet ChooseChatHeaderSheet(const bool compact, const bool group_chat) {
  return compact && group_chat ? ChatHeaderSheet::GroupInfo : ChatHeaderSheet::Menu;
}

bool GroupInfoSheet::Register(ui::Context* context) {
  context_ = context;
  return DataModelHost::Instance().Register(context, kModel, [this](ui::DataModelConstructor& ctor) {
    if (auto row = ctor.RegisterStruct<GroupInfoMemberRow>()) {
      row.RegisterMember("identity", &GroupInfoMemberRow::identity);
      row.RegisterMember("title", &GroupInfoMemberRow::title);
      row.RegisterMember("letter", &GroupInfoMemberRow::letter);
      row.RegisterMember("tone", &GroupInfoMemberRow::tone);
      row.RegisterMember("owner", &GroupInfoMemberRow::owner);
      row.RegisterMember("admin", &GroupInfoMemberRow::admin);
      row.RegisterMember("unreachable", &GroupInfoMemberRow::unreachable);
      row.RegisterMember("self", &GroupInfoMemberRow::self);
    }
    ctor.RegisterArray<std::vector<GroupInfoMemberRow>>();
    if (auto link = ctor.RegisterStruct<GroupInfoLinkRow>()) {
      link.RegisterMember("link_id", &GroupInfoLinkRow::link_id);
      link.RegisterMember("label", &GroupInfoLinkRow::label);
      link.RegisterMember("detail", &GroupInfoLinkRow::detail);
      link.RegisterMember("can_revoke", &GroupInfoLinkRow::can_revoke);
    }
    ctor.RegisterArray<std::vector<GroupInfoLinkRow>>();
    ctor.Bind("title", &view_.title);
    ctor.Bind("count_label", &view_.count_label);
    ctor.Bind("dissolved", &view_.dissolved);
    ctor.Bind("muted", &view_.muted);
    ctor.Bind("show_members", &view_.show_members);
    ctor.Bind("show_calls", &view_.show_calls);
    ctor.Bind("show_rename", &view_.show_rename);
    ctor.Bind("show_restore_name", &view_.show_restore_name);
    ctor.Bind("show_add_member", &view_.show_add_member);
    ctor.Bind("show_leave", &view_.show_leave);
    ctor.Bind("show_dissolve", &view_.show_dissolve);
    ctor.Bind("show_preset", &view_.show_preset);
    ctor.Bind("preset_editable", &view_.preset_editable);
    ctor.Bind("preset_label", &view_.preset_label);
    ctor.Bind("members_label", &view_.members_label);
    ctor.Bind("view_all_label", &view_.view_all_label);
    ctor.Bind("members", &view_.members);
    ctor.Bind("show_invite_link", &view_.show_invite_link);
    ctor.Bind("show_links", &view_.show_links);
    ctor.Bind("links", &view_.links);
    ctor.BindEventCallback("group_info_voice", DataEvent([this] { Call(/*video=*/false); }));
    ctor.BindEventCallback("group_info_video", DataEvent([this] { Call(/*video=*/true); }));
    ctor.BindEventCallback("group_info_mute", DataEvent([this] { Mute(); }));
    ctor.BindEventCallback("group_info_rename", DataEvent([this] { Rename(); }));
    ctor.BindEventCallback("group_info_restore_name", DataEvent([this] { RestoreName(); }));
    ctor.BindEventCallback("group_info_view_all", DataEvent([this] { ViewAll(); }));
    ctor.BindEventCallback("group_info_add_member", DataEvent([this] { AddMember(); }));
    ctor.BindEventCallback("group_info_preset", DataEvent([this] { ChoosePreset(); }));
    ctor.BindEventCallback("group_info_member",
                           DataEvent<std::string>([this](const std::string& identity) { TapMember(identity); }));
    ctor.BindEventCallback("group_info_leave", DataEvent([this] { Leave(); }));
    ctor.BindEventCallback("group_info_dissolve", DataEvent([this] { Dissolve(); }));
    ctor.BindEventCallback("group_info_invite_link", DataEvent([this] { InviteLink(); }));
    ctor.BindEventCallback("group_info_revoke_link",
                           DataEvent<std::string>([this](const std::string& link_id) { RevokeLink(link_id); }));
  });
}

void GroupInfoSheet::DirtyAll() {
  if (context_) {
    DataModelHost::Instance().DirtyAll(kModel);
  }
}

bool GroupInfoSheet::Open(const std::string& thread_id) {
  if (!host_.GroupInfoCompact() || thread_id.empty()) {
    return false;
  }
  const GroupInfoScreen screen = host_.GroupInfoRead(thread_id);
  if (screen.thread_id.empty()) {
    return false;
  }
  if (IsOpen() && thread_id_ != thread_id) {
    Close();
  }
  thread_id_ = thread_id;
  Apply(screen);
  last_read_ = std::chrono::steady_clock::now();
  if (!IsOpen()) {
    PaneSpec spec;
    spec.key = "group_info";
    spec.page_sheet = true; // the sheets' one height, handle, centred title and grey X (Kenneth 2026-11-04)
    layer_id_ = host_.GroupInfoPushSheet(spec);
  }
  return IsOpen();
}

void GroupInfoSheet::Close() {
  const int layer_id = layer_id_;
  layer_id_ = -1;
  if (layer_id >= 0) {
    host_.GroupInfoCloseSheet(layer_id);
  }
}

void GroupInfoSheet::Refresh() {
  if (!IsOpen()) {
    return;
  }
  Apply(host_.GroupInfoRead(thread_id_));
}

void GroupInfoSheet::Tick(const std::string& active_thread_id, const std::chrono::steady_clock::time_point now) {
  if (!IsOpen()) {
    return;
  }
  if (!host_.GroupInfoSheetOpen(layer_id_)) {
    layer_id_ = -1; // closed by its X, the scrim or a swipe
    return;
  }
  if (active_thread_id != thread_id_) {
    Close(); // the chat was left (Exit group) or another opened (Message from a member): the sheets go with it
    return;
  }
  if (now - last_read_ >= kRereadEvery) {
    last_read_ = now;
    Refresh();
  }
}

void GroupInfoSheet::Apply(const GroupInfoScreen& screen) {
  if (screen.thread_id.empty()) {
    Close(); // no longer a group this device can show (the chat was closed)
    return;
  }
  if (screen == screen_ && !view_.title.empty()) {
    return;
  }
  screen_ = screen;
  const std::string count = std::to_string(screen.member_count);
  view_.title = screen.title.c_str();
  view_.dissolved = screen.dissolved;
  view_.count_label = screen.dissolved ? "" : Tr("group_info.people", {{"count", count}}).c_str();
  view_.muted = screen.muted;
  view_.show_members = screen.show_members;
  view_.show_calls = screen.show_calls;
  view_.show_rename = screen.show_rename;
  view_.show_restore_name = screen.show_restore_name;
  view_.show_add_member = screen.show_add_member;
  view_.show_leave = screen.show_leave;
  view_.show_dissolve = screen.show_dissolve;
  view_.show_preset = screen.show_preset;
  view_.preset_editable = screen.preset_editable;
  view_.preset_label =
      Tr(screen.preset == GroupPreset::Managed ? "group_info.preset_managed" : "group_info.preset_open").c_str();
  view_.members_label = Tr("group_info.members", {{"count", count}}).c_str();
  view_.view_all_label = Tr("group_info.view_all", {{"count", count}}).c_str();
  view_.members.clear();
  for (const GroupMemberRow& member : screen.preview) {
    GroupInfoMemberRow row;
    row.identity = member.identity.c_str();
    row.title = member.title.c_str();
    const AvatarGlyph glyph = MakeAvatarGlyph(member.title, member.stable_id);
    row.letter = glyph.letter.c_str();
    row.tone = glyph.tone;
    row.owner = member.is_owner;
    row.admin = member.is_admin;
    row.unreachable = member.unreachable;
    row.self = !screen.self_identity.empty() && member.identity == screen.self_identity;
    view_.members.push_back(std::move(row));
  }
  view_.show_invite_link = screen.show_invite_link;
  view_.show_links = screen.show_links;
  view_.links.clear();
  for (const GroupLinkListRow& link : screen.links) {
    view_.links.push_back({.link_id = link.link_id.c_str(),
                           .label = link.label.c_str(),
                           .detail = link.detail.c_str(),
                           .can_revoke = link.can_revoke});
  }
  DirtyAll();
}

void GroupInfoSheet::Call(const bool video) {
  if (IsOpen() && screen_.show_calls) {
    host_.GroupInfoCall(thread_id_, video);
  }
}

void GroupInfoSheet::Mute() {
  if (IsOpen()) {
    host_.GroupInfoMute(thread_id_, screen_.muted);
  }
}

void GroupInfoSheet::Rename() {
  if (IsOpen() && screen_.show_rename) {
    host_.GroupInfoRename(thread_id_, screen_.rename_for_everyone);
  }
}

void GroupInfoSheet::RestoreName() {
  if (IsOpen() && screen_.show_restore_name) {
    host_.GroupInfoRestoreName(thread_id_);
    Refresh();
  }
}

void GroupInfoSheet::ViewAll() {
  if (IsOpen() && screen_.show_members) {
    host_.GroupInfoViewAll(thread_id_);
  }
}

void GroupInfoSheet::AddMember() {
  if (IsOpen() && screen_.show_add_member) {
    host_.GroupInfoAddMember(thread_id_);
  }
}

void GroupInfoSheet::ChoosePreset() {
  if (IsOpen() && screen_.show_preset && screen_.preset_editable) {
    host_.GroupInfoChoosePreset(screen_.group_id, screen_.preset);
  }
}

void GroupInfoSheet::TapMember(const std::string& identity) {
  if (!IsOpen() || identity.empty() || identity == screen_.self_identity) {
    return; // our own row does nothing
  }
  host_.GroupInfoTapMember(thread_id_, identity);
}

void GroupInfoSheet::Leave() {
  if (IsOpen() && screen_.show_leave) {
    host_.GroupInfoLeave(thread_id_);
  }
}

void GroupInfoSheet::Dissolve() {
  if (IsOpen() && screen_.show_dissolve) {
    host_.GroupInfoDissolve(screen_.group_id);
  }
}

void GroupInfoSheet::InviteLink() {
  if (IsOpen() && screen_.show_invite_link) {
    host_.GroupInfoInviteLink(screen_.group_id);
  }
}

void GroupInfoSheet::RevokeLink(const std::string& link_id) {
  if (!IsOpen() || link_id.empty()) {
    return;
  }
  for (const GroupLinkListRow& link : screen_.links) {
    if (link.link_id == link_id && link.can_revoke) {
      host_.GroupInfoRevokeLink(screen_.group_id, link_id);
      return;
    }
  }
}

} // namespace pbr
