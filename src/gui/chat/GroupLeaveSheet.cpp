#include "gui/chat/GroupLeaveSheet.h"

#include "gui/shell/DataModelHost.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/ui/DataEvent.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>

#include <algorithm>

namespace pbr {

namespace {

constexpr const char* kModel = "group_leave";
constexpr auto kRereadEvery = std::chrono::seconds(1);

} // namespace

bool GroupLeaveSheet::Register(ui::Context* context) {
  context_ = context;
  return DataModelHost::Instance().Register(context, kModel, [this](ui::DataModelConstructor& ctor) {
    if (auto row = ctor.RegisterStruct<GroupLeaveRow>()) {
      row.RegisterMember("identity", &GroupLeaveRow::identity);
      row.RegisterMember("name", &GroupLeaveRow::name);
      row.RegisterMember("letter", &GroupLeaveRow::letter);
      row.RegisterMember("tone", &GroupLeaveRow::tone);
    }
    ctor.RegisterArray<std::vector<GroupLeaveRow>>();
    ctor.Bind("empty", &view_.empty);
    ctor.Bind("rows", &view_.rows);
    ctor.BindEventCallback("group_leave_pick",
                           DataEvent<std::string>([this](const std::string& identity) { Pick(identity); }));
    ctor.BindEventCallback("group_leave_dissolve", DataEvent([this] { Dissolve(); }));
  });
}

void GroupLeaveSheet::DirtyAll() {
  if (context_) {
    DataModelHost::Instance().DirtyAll(kModel);
  }
}

bool GroupLeaveSheet::Open(const std::string& thread_id) {
  const OwnerLeaveSheet sheet = host_.GroupLeaveRead(thread_id);
  if (sheet.thread_id.empty()) {
    return false;
  }
  if (IsOpen() && thread_id_ != thread_id) {
    Close();
  }
  thread_id_ = thread_id;
  Apply(sheet);
  last_read_ = std::chrono::steady_clock::now();
  if (!IsOpen()) {
    PaneSpec spec;
    spec.key = "group_leave";
    spec.page_sheet = true;
    layer_id_ = host_.GroupLeavePushSheet(spec);
  }
  return IsOpen();
}

void GroupLeaveSheet::Close() {
  const int layer_id = layer_id_;
  layer_id_ = -1;
  if (layer_id >= 0) {
    host_.GroupLeaveCloseSheet(layer_id);
  }
}

void GroupLeaveSheet::Tick(const std::chrono::steady_clock::time_point now) {
  if (!IsOpen()) {
    return;
  }
  if (!host_.GroupLeaveSheetOpen(layer_id_)) {
    layer_id_ = -1;
    return;
  }
  if (now - last_read_ >= kRereadEvery) {
    last_read_ = now;
    Refresh();
  }
}

void GroupLeaveSheet::Refresh() {
  const OwnerLeaveSheet sheet = host_.GroupLeaveRead(thread_id_);
  if (sheet.thread_id.empty()) {
    Close(); // the group was dissolved or handed over meanwhile
    return;
  }
  Apply(sheet);
}

void GroupLeaveSheet::Apply(const OwnerLeaveSheet& sheet) {
  if (sheet == sheet_ && layer_id_ >= 0) {
    return;
  }
  sheet_ = sheet;
  view_.empty = sheet.candidates.empty();
  view_.rows.clear();
  for (const OwnerLeaveCandidate& candidate : sheet.candidates) {
    GroupLeaveRow row;
    row.identity = candidate.identity.c_str();
    row.name = candidate.name.c_str();
    const AvatarGlyph glyph = MakeAvatarGlyph(candidate.name, candidate.identity);
    row.letter = glyph.letter.c_str();
    row.tone = glyph.tone;
    view_.rows.push_back(std::move(row));
  }
  DirtyAll();
}

void GroupLeaveSheet::Pick(const std::string& identity) {
  const auto found = std::find_if(sheet_.candidates.begin(), sheet_.candidates.end(),
                                  [&identity](const OwnerLeaveCandidate& c) { return c.identity == identity; });
  if (!IsOpen() || found == sheet_.candidates.end()) {
    return;
  }
  const std::string name = found->name;
  const std::string thread_id = thread_id_;
  const std::string group_id = sheet_.group_id;
  // Asked once, at the end, naming the person (the first "Leave?" of the old flow is gone).
  host_.GroupLeaveConfirm(HandOverConfirmText(name), [this, thread_id, group_id, identity, name](const bool ok) {
    if (!ok) {
      return;
    }
    if (!host_.GroupLeaveStillPossible(thread_id, identity)) {
      host_.GroupLeaveToast(HandOverGoneText(name));
      if (IsOpen() && thread_id_ == thread_id) {
        Refresh();
      }
      return;
    }
    Close();
    host_.GroupLeaveHandOver(thread_id, group_id, identity);
  });
}

void GroupLeaveSheet::Dissolve() {
  if (IsOpen()) {
    host_.GroupLeaveDissolve(sheet_.group_id); // its own confirm; the sheet closes once the group has ended
  }
}

} // namespace pbr
