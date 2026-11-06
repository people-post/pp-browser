#include "gui/chat/GroupLinkSheet.h"

#include "feature/presenters/GroupLinkText.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/QrSvg.h"

#include "common/Utilities.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>

namespace pbr {

namespace {

constexpr const char* kModel = "group_link";

} // namespace

bool GroupLinkSheet::Register(ui::Context* context) {
  context_ = context;
  return DataModelHost::Instance().Register(context, kModel, [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("loading", &view_.loading);
    ctor.Bind("text", &view_.text);
    ctor.Bind("qr_svg_rml", &view_.qr_svg_rml);
    ctor.Bind("expiry", &view_.expiry);
    ctor.Bind("error", &view_.error);
    ctor.Bind("can_revoke", &view_.can_revoke);
    ctor.BindEventCallback("group_link_copy", DataEvent([this] { Copy(); }));
    ctor.BindEventCallback("group_link_revoke", DataEvent([this] { Revoke(); }));
  });
}

void GroupLinkSheet::Dirty() {
  if (context_) {
    DataModelHost::Instance().DirtyAll(kModel);
  }
}

void GroupLinkSheet::Open(const std::string& group_id) {
  if (group_id.empty() || !ports_.push_sheet || !ports_.load) {
    return;
  }
  group_id_ = group_id;
  link_id_.clear();
  text_.clear();
  view_ = GroupLinkView{.loading = true};
  Dirty();
  if (!IsOpen()) {
    PaneSpec spec;
    spec.key = "group_link";
    spec.page_sheet = true;
    layer_id_ = ports_.push_sheet(spec);
  }
  const uint64_t opening = ++opening_;
  ports_.load(group_id, [this, opening](Roe<GroupLinkRow> link) {
    if (opening == opening_ && IsOpen()) {
      const bool shown = static_cast<bool>(link);
      Show(std::move(link));
      if (shown && ports_.changed) {
        ports_.changed(); // a new link is in the group's list
      }
    }
  });
}

void GroupLinkSheet::Show(Roe<GroupLinkRow> link) {
  view_.loading = false;
  if (!link) {
    view_.error = link.error().message.c_str();
    Dirty();
    return;
  }
  link_id_ = link->link_id;
  text_ = link->text;
  view_.text = text_.c_str();
  auto qr = QrSvgContent(text_);
  view_.qr_svg_rml = qr ? qr->c_str() : "";
  view_.expiry = GroupLinkExpiryText(link->expires_at, util::NowUnixMs()).c_str();
  view_.can_revoke = link->can_revoke;
  Dirty();
}

void GroupLinkSheet::Close() {
  const int layer_id = layer_id_;
  layer_id_ = -1;
  ++opening_;
  if (layer_id >= 0 && ports_.close_sheet) {
    ports_.close_sheet(layer_id);
  }
}

void GroupLinkSheet::Tick() {
  if (IsOpen() && ports_.sheet_open && !ports_.sheet_open(layer_id_)) {
    layer_id_ = -1;
    ++opening_;
  }
}

void GroupLinkSheet::Copy() {
  if (IsOpen() && !text_.empty() && ports_.copied) {
    ports_.copied(text_);
  }
}

void GroupLinkSheet::Revoke() {
  if (!IsOpen() || link_id_.empty() || !view_.can_revoke || !ports_.revoke) {
    return;
  }
  ports_.revoke(group_id_, link_id_, [this](const bool revoked) {
    if (!revoked) {
      return;
    }
    Close();
    if (ports_.changed) {
      ports_.changed();
    }
  });
}

} // namespace pbr
