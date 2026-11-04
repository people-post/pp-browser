#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "lib/ui/ShellTypes.h"

#include <ui/base/Types.h>

#include <chrono>
#include <string>
#include <vector>

namespace ui {
class Context;
}

namespace pbr {

/** One person on the sheet's member card, as the RML binds it. */
struct GroupInfoMemberRow {
  ui::String identity;
  ui::String title;
  ui::String letter;
  int tone = 0;
  bool owner = false;
  bool unreachable = false;
  /** This device's own row: shown, not tappable. */
  bool self = false;
};

/** What the sheet binds ("group_info" data model). */
struct GroupInfoView {
  ui::String title;
  /** "5 people" under the name; "" for a dissolved group. */
  ui::String count_label;
  bool dissolved = false;
  bool muted = false;
  bool show_members = false;
  bool show_calls = false;
  bool show_rename = false;
  bool show_restore_name = false;
  bool show_add_member = false;
  bool show_leave = false;
  bool show_dissolve = false;
  ui::String members_label;
  ui::String view_all_label;
  std::vector<GroupInfoMemberRow> members;
};

/** What the sheet needs from the chat screen (ChatController); every action is an existing one. UI thread. */
class GroupInfoSheetHost {
public:
  virtual ~GroupInfoSheetHost() = default;
  /** The sheet is a phone surface; the desktop keeps the header's small menu. */
  virtual bool GroupInfoCompact() const = 0;
  virtual GroupInfoScreen GroupInfoRead(const std::string& thread_id) = 0;
  virtual int GroupInfoPushSheet(const PaneSpec& spec) = 0;
  virtual void GroupInfoCloseSheet(int layer_id) = 0;
  /** The layer is still open (its X, the scrim or a swipe closes it without telling anyone). */
  virtual bool GroupInfoSheetOpen(int layer_id) const = 0;
  /** Voice / video: the chat's own call button (joins the call that goes on, else the member picker). */
  virtual void GroupInfoCall(const std::string& thread_id, bool video) = 0;
  /** Mute: 8 hours / 1 week / Always, and Unmute when `muted`, as an action sheet. */
  virtual void GroupInfoMute(const std::string& thread_id, bool muted) = 0;
  virtual void GroupInfoRename(const std::string& thread_id, bool for_everyone) = 0;
  virtual void GroupInfoRestoreName(const std::string& thread_id) = 0;
  virtual void GroupInfoViewAll(const std::string& thread_id) = 0;
  virtual void GroupInfoAddMember(const std::string& thread_id) = 0;
  virtual void GroupInfoTapMember(const std::string& thread_id, const std::string& identity) = 0;
  /** Leave: the chat's close flow (an owner hands the group over or dissolves it). */
  virtual void GroupInfoLeave(const std::string& thread_id) = 0;
  virtual void GroupInfoDissolve(const std::string& group_id) = 0;
};

/**
 * The phone's group info sheet (DEV/PLAN-ui-batch8-group-pages.md 1, mockup 06-GroupInfo): opened from a group chat's
 * title. The group's photo and name, voice / video / mute, its name, the first members, Add member (the owner), See
 * all, Leave and (the owner) Dissolve. The rows are ChatPresenter's (GroupInfoScreen); each button runs the action the
 * chat already has. Sheets, menus and dialogs it opens stack over it, so closing them comes back here.
 *
 * UI thread.
 */
class GroupInfoSheet {
public:
  explicit GroupInfoSheet(GroupInfoSheetHost& host) : host_(host) {}

  bool Register(ui::Context* context);

  /** Opens over group chat `thread_id` (false: not a phone, or not a group). */
  bool Open(const std::string& thread_id);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  const std::string& ThreadId() const { return thread_id_; }
  /** Reads the group again; dirties the model only when something shows differently. */
  void Refresh();
  /**
   * Per frame: notices the sheet was closed (X, scrim, swipe), closes it when another chat (or none) is open now,
   * and re-reads the group once a second (members, names and mute change underneath it).
   */
  void Tick(const std::string& active_thread_id, std::chrono::steady_clock::time_point now);

  // --- the sheet's buttons ---------------------------------------------------------------------------
  void Call(bool video);
  void Mute();
  void Rename();
  void RestoreName();
  void ViewAll();
  void AddMember();
  void TapMember(const std::string& identity);
  void Leave();
  void Dissolve();

  const GroupInfoView& View() const { return view_; }
  const GroupInfoScreen& Screen() const { return screen_; }

private:
  void Apply(const GroupInfoScreen& screen);
  void DirtyAll();

  GroupInfoSheetHost& host_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  std::string thread_id_;
  GroupInfoScreen screen_;
  GroupInfoView view_;
  std::chrono::steady_clock::time_point last_read_{};
};

/** What the chat title opens: the group info sheet (a group chat on a phone), else the small menu. */
enum class ChatHeaderSheet { Menu, GroupInfo };
ChatHeaderSheet ChooseChatHeaderSheet(bool compact, bool group_chat);

} // namespace pbr
