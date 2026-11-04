#pragma once

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

#include <optional>

namespace ui {
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * Home's 关注联系人 row (FavoritesRowModel): once its sideways scroll has come to rest (the finger is up and no scroll
 * event came for FavoritesRowModel::kRestMs, i.e. the coast is over), it eases to the nearest person so four whole
 * avatars show. Driven by the row's own scroll events and one timed wake (ShellHost asks for it at frame end): no
 * per-frame polling while nothing moves.
 */
class ShellFavoritesSnap : public ui::EventListener {
public:
  ~ShellFavoritesSnap() override { Detach(); }

  void Attach(ui::ElementDocument* document);
  void Detach();

  /** Seconds until a moved row counts as at rest (the frame to wake for); none when nothing waits. */
  std::optional<double> SecondsUntilRest(double now_s) const;
  /** Eases a row that has come to rest onto a person boundary. */
  void Update(double now_s);

  void ProcessEvent(ui::Event& event) override;

  static double NowSeconds();

private:
  ui::ObserverPtr<ui::Element> document_;
  ui::ObserverPtr<ui::Element> row_;
  bool pressed_ = false;
  bool pending_ = false;
  double last_move_s_ = 0.0;
};

} // namespace pbr
