#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "lib/ui/ShellTypes.h"

#include <ui/base/Types.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ui {
class Context;
}

namespace pbr {

/** One person the owner may hand the group to, as the RML binds it. */
struct GroupLeaveRow {
  ui::String identity;
  ui::String name;
  ui::String letter;
  int tone = 0;
};

/** What the sheet binds ("group_leave" data model). */
struct GroupLeaveView {
  /** Nobody can take over now: the sheet says so and offers only Dissolve. */
  bool empty = false;
  std::vector<GroupLeaveRow> rows;
};

/** What the sheet needs from the chat screen (ChatController). UI thread. */
class GroupLeaveSheetHost {
public:
  virtual ~GroupLeaveSheetHost() = default;
  virtual OwnerLeaveSheet GroupLeaveRead(const std::string& thread_id) = 0;
  virtual int GroupLeavePushSheet(const PaneSpec& spec) = 0;
  virtual void GroupLeaveCloseSheet(int layer_id) = 0;
  virtual bool GroupLeaveSheetOpen(int layer_id) const = 0;
  /** The iOS alert, its confirm in red (handing the group over and leaving cannot be undone). */
  virtual void GroupLeaveConfirm(const std::string& message, std::function<void(bool ok)> done) = 0;
  /** Asked again right before handing over: the one picked can still take it. */
  virtual bool GroupLeaveStillPossible(const std::string& thread_id, const std::string& identity) = 0;
  /** The existing hand-over: LeaveAsOwner, then the chat closes here. */
  virtual void GroupLeaveHandOver(const std::string& thread_id, const std::string& group_id,
                                  const std::string& identity) = 0;
  /** The existing dissolve, with its own confirm. */
  virtual void GroupLeaveDissolve(const std::string& group_id) = 0;
  /** The bottom pill. */
  virtual void GroupLeaveToast(const std::string& text) = 0;
};

/**
 * The owner leaving a group on a phone (DEV/PLAN-ui-batch8-group-pages.md 3, mockup 06-GroupLeave): a 4/5 page sheet
 * "Choose a new owner" listing everyone who can take over (reachable members only), and a red "Dissolve the group
 * instead". A pick asks once, naming the person ("Make X the owner and exit the group?"); confirmed, it checks again
 * that X can still take over (else "X is no longer in the group" and the list is read again) and hands over. With
 * nobody to take over it says so and offers only Dissolve. The rules are ChatPresenter's.
 *
 * UI thread.
 */
class GroupLeaveSheet {
public:
  explicit GroupLeaveSheet(GroupLeaveSheetHost& host) : host_(host) {}

  bool Register(ui::Context* context);

  /** Opens for the owner leaving `thread_id` (false: the owner is not leaving that chat). */
  bool Open(const std::string& thread_id);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  /** Per frame: notices it was closed, and closes once the group no longer needs it (dissolved, handed over). */
  void Tick(std::chrono::steady_clock::time_point now);

  // --- the sheet's buttons ---------------------------------------------------------------------------
  void Pick(const std::string& identity);
  void Dissolve();

  const GroupLeaveView& View() const { return view_; }

private:
  void Refresh();
  void Apply(const OwnerLeaveSheet& sheet);
  void DirtyAll();

  GroupLeaveSheetHost& host_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  std::string thread_id_;
  OwnerLeaveSheet sheet_;
  GroupLeaveView view_;
  std::chrono::steady_clock::time_point last_read_{};
};

} // namespace pbr
