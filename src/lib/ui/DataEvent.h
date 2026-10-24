#pragma once

#include "lib/ui/RmlVariantHelpers.h"

#include <ui/data/DataModelHandle.h>
#include <ui/data/DataTypes.h>
#include <ui/dom/Event.h>

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace pbr {

namespace data_event_detail {

template <typename T>
std::optional<T> Arg(const ui::VariantList& args, size_t index);

template <>
inline std::optional<std::string> Arg<std::string>(const ui::VariantList& args, size_t index) {
  return EventArgAsString(args, index);
}

template <>
inline std::optional<int> Arg<int>(const ui::VariantList& args, size_t index) {
  return EventArgAsInt(args, index);
}

template <typename... Args, typename F, size_t... I>
void Invoke(F& handler, [[maybe_unused]] const ui::VariantList& args, std::index_sequence<I...> /*indices*/) {
  std::tuple<std::optional<Args>...> values{Arg<Args>(args, I)...};
  if (!(std::get<I>(values).has_value() && ...)) {
    return;
  }
  handler(*std::get<I>(values)...);
}

} // namespace data_event_detail

/**
 * A data-model event handler from a callable that takes the event's arguments, typed:
 *
 *   ctor.BindEventCallback("toggle_reaction", DataEvent<std::string, std::string>(
 *       [this](const std::string& message_id, const std::string& emoji) { ToggleReaction(message_id, emoji); }));
 *
 * The callable runs only when the event carries every argument with its type (`std::string`: a string;
 * `int`: a number or a numeric string). With no types it takes no arguments, and extra ones are ignored.
 */
template <typename... Args, typename F>
ui::DataEventFunc DataEvent(F handler) {
  return [handler = std::move(handler)](ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                        const ui::VariantList& args) mutable {
    data_event_detail::Invoke<Args...>(handler, args, std::index_sequence_for<Args...>{});
  };
}

/** A handler that needs the event itself (to anchor a menu under the clicked element). */
template <typename F>
ui::DataEventFunc DataEventAt(F handler) {
  return [handler = std::move(handler)](ui::DataModelHandle /*model*/, ui::Event& ev,
                                        const ui::VariantList& /*args*/) mutable { handler(ev); };
}

} // namespace pbr
