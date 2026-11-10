#include "gui/shell/ShellFavoritesSnap.h"

#include "gui/shell/FavoritesRowModel.h"

#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/dom/ScrollTypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pbr {

namespace {

constexpr const char* kRowClass = "home-avs";
constexpr const char* kPersonClass = "home-av";

bool InRow(ui::Element* element) {
  for (ui::Element* e = element; e; e = e->GetParentNode()) {
    if (e->IsClassSet(kRowClass)) {
      return true;
    }
  }
  return false;
}

} // namespace

double ShellFavoritesSnap::NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ShellFavoritesSnap::Attach(ui::ElementDocument* document) {
  Detach();
  if (!document) {
    return;
  }
  document_ = document->GetObserverPtr();
  document->AddEventListener(ui::EventId::Scroll, this);
  document->AddEventListener(ui::EventId::Mousedown, this);
  document->AddEventListener(ui::EventId::Mouseup, this);
}

void ShellFavoritesSnap::Detach() {
  if (document_) {
    document_->RemoveEventListener(ui::EventId::Scroll, this);
    document_->RemoveEventListener(ui::EventId::Mousedown, this);
    document_->RemoveEventListener(ui::EventId::Mouseup, this);
  }
  document_ = nullptr;
  row_ = nullptr;
  pressed_ = false;
  pending_ = false;
}

void ShellFavoritesSnap::ProcessEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  switch (event.GetId()) {
    case ui::EventId::Scroll:
      if (target && target->IsClassSet(kRowClass)) {
        row_ = target->GetObserverPtr();
        pending_ = true;
        last_move_s_ = NowSeconds();
      }
      break;
    case ui::EventId::Mousedown:
      pressed_ = InRow(target);
      break;
    case ui::EventId::Mouseup:
      // The finger is up: a coast may follow; the rest timer starts again from here.
      if (pressed_ && pending_) {
        last_move_s_ = NowSeconds();
      }
      pressed_ = false;
      break;
    default:
      break;
  }
}

std::optional<double> ShellFavoritesSnap::SecondsUntilRest(const double now_s) const {
  if (!pending_ || pressed_) {
    return std::nullopt;
  }
  const double rest_s = FavoritesRowModel::kRestMs / 1000.0;
  return std::max(0.0, rest_s - (now_s - last_move_s_));
}

void ShellFavoritesSnap::Update(const double now_s) {
  if (!pending_ || pressed_ || now_s - last_move_s_ < FavoritesRowModel::kRestMs / 1000.0) {
    return;
  }
  pending_ = false;
  ui::Element* row = row_.get();
  if (!row) {
    return;
  }
  size_t count = 0;
  for (int i = 0; i < row->GetNumChildren(); ++i) {
    if (row->GetChild(i)->IsClassSet(kPersonClass)) {
      ++count;
    }
  }
  const float cell = row->GetClientWidth() / static_cast<float>(FavoritesRowModel::kVisible);
  const float left = row->GetScrollLeft();
  const float target = FavoritesRowModel::SnapOffset(left, cell, count);
  if (std::abs(target - left) > 0.5f) {
    // The ease's own scroll events arm this again; at rest on the boundary it then does nothing.
    row->ScrollTo({target, row->GetScrollTop()}, ui::ScrollBehavior::Smooth);
  }
}

} // namespace pbr
