#pragma once

#include <string>

namespace pbr {

/**
 * What a call-media error says happened, decided where it happened (K014). The call screen and the notice
 * layer read the kind, never the message's text: the message is for the log, and is the words of an Other.
 */
enum class CallMediaErrorKind {
  /** Told by its words only (the hop path's, the 1:1 connect sequence's, a feature's localized line). */
  Other,
  /** The 1:1 call never connected within its connect budget: the call screen's "Couldn't reach them" page. */
  ConnectTimeout,
  /**
   * The call was connected, then lost its media for good (`CallMediaErr::Dropped`, or the health tick's
   * backstop for a stream gone with no report): the call screen's "Call dropped" page. Never a connect failure.
   */
  Dropped,
};

/** A call-media error for the screen to show once (CallDevice → CallUiBackend::TakeLastMediaError). */
struct CallMediaError {
  CallMediaErrorKind kind = CallMediaErrorKind::Other;
  /** The raw reason (the log); an Other's words for the notice. */
  std::string message;

  bool operator==(const CallMediaError&) const = default;
};

} // namespace pbr
