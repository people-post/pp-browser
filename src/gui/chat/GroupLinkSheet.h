#pragma once

#include "feature/conversations/GroupInviteLinkWorkflow.h"
#include "lib/ui/ShellTypes.h"

#include "common/Error.h"

#include <ui/base/Types.h>

#include <functional>
#include <string>

namespace ui {
class Context;
}

namespace pbr {

/** What the invite link sheet binds ("group_link" data model). */
struct GroupLinkView {
  bool loading = false;
  /** `pp://join/…`, selectable; empty while loading or when it failed. */
  ui::String text;
  /** The link's QR code (QrSvgContent), for `data-rml`. */
  ui::String qr_svg_rml;
  ui::String expiry;
  ui::String error;
  bool can_revoke = false;
};

/** What the sheet needs from the chat screen. UI thread; the two blocking calls run on a worker and answer here. */
struct GroupLinkSheetPorts {
  std::function<int(const PaneSpec& spec)> push_sheet;
  std::function<void(int layer_id)> close_sheet;
  std::function<bool(int layer_id)> sheet_open;
  /** Our link to the group (ChatPresenter::InviteLinkFor: ours that lives, else a new one). */
  std::function<void(const std::string& group_id, std::function<void(Roe<GroupLinkRow>)> done)> load;
  /** Ask, then revoke (ChatPresenter::RevokeInviteLink); `done(true)` once it is revoked. */
  std::function<void(const std::string& group_id, const std::string& link_id, std::function<void(bool)> done)> revoke;
  std::function<void(const std::string& text)> copied;
  /** The group's rows changed (a link made or revoked): group info reads again. */
  std::function<void()> changed;
};

/**
 * The "Invite link…" sheet (G021): over group info, our link to the group — its text to copy, its QR code to scan,
 * when it expires — and "Revoke link…". Opening it shows our live link, or makes one (7 days). UI thread.
 */
class GroupLinkSheet {
public:
  explicit GroupLinkSheet(GroupLinkSheetPorts ports) : ports_(std::move(ports)) {}

  bool Register(ui::Context* context);
  void Open(const std::string& group_id);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  /** Per frame: notices the sheet was closed by its X, the scrim or a swipe. */
  void Tick();

  void Copy();
  void Revoke();

  const GroupLinkView& View() const { return view_; }

private:
  void Show(Roe<GroupLinkRow> link);
  void Dirty();

  GroupLinkSheetPorts ports_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  std::string group_id_;
  std::string link_id_;
  std::string text_;
  /** Bumped per Open: an answer for an earlier opening is dropped. */
  uint64_t opening_ = 0;
  GroupLinkView view_;
};

} // namespace pbr
