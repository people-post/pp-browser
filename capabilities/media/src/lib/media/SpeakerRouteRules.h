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

} // namespace pbr
