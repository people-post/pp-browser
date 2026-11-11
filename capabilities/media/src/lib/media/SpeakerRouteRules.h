#pragma once

namespace pbr {

/** What the OS route was last set to by the call's audio session: not yet, the earpiece, the loudspeaker. */
enum class RoutedSpeaker { Unknown, Off, On };

/**
 * Whether activating the call's audio session pushes the speaker choice to the OS (Android AudioSession). Always on
 * the first activation; afterwards only when the route differs from the choice — a call that ended forgets its choice
 * without routing (ForgetSpeakerChoice), and the next call's activation, which runs before its mic opens, routes back.
 * Re-pushing an unchanged route races OEM route changes (Moto) and can kill an AAudio capture mid-open.
 */
inline bool PushSpeakerRouteOnActivate(const bool was_active, const RoutedSpeaker routed, const bool wanted) {
  if (!was_active) {
    return true;
  }
  return routed != (wanted ? RoutedSpeaker::On : RoutedSpeaker::Off);
}

/**
 * A call ended (ForgetSpeakerChoice): whether the session routes back to the earpiece right away (Android). Only
 * while the session is still active (else Deactivate already did) and no mic is open — a route change under an open
 * recording stream loses the mic for the process. With a mic open it is left to Deactivate, which runs after that
 * mic has closed, or to the next activation (PushSpeakerRouteOnActivate). Right away matters when no engine ever
 * took the session over: a ringback the user put on the speaker, the call gone before media — nothing else would
 * route back, and the next ring or voice clip would play on the loudspeaker.
 */
inline bool RouteBackWhenChoiceForgotten(const bool session_active, const bool mic_open) {
  return session_active && !mic_open;
}

} // namespace pbr
