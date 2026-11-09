#include "lib/media/AudioFocusInterruption.h"

namespace pbr {

namespace {

// android.media.AudioManager.
constexpr int kAudioFocusGain = 1;
constexpr int kAudioFocusGainTransientExclusive = 4;
constexpr int kAudioFocusLoss = -1;
constexpr int kAudioFocusLossTransient = -2;
constexpr int kAudioFocusLossTransientCanDuck = -3;

} // namespace

AudioFocusInterruption::Change AudioFocusInterruption::FromAndroid(const int focus_change) {
  if (focus_change >= kAudioFocusGain && focus_change <= kAudioFocusGainTransientExclusive) {
    return Change::Gain;
  }
  switch (focus_change) {
    case kAudioFocusLoss:
      return Change::Loss;
    case kAudioFocusLossTransient:
      return Change::LossTransient;
    case kAudioFocusLossTransientCanDuck:
      return Change::LossTransientCanDuck;
    default:
      return Change::Unknown;
  }
}

AudioFocusInterruption::Step AudioFocusInterruption::OnChange(const Change change, const bool session_active,
                                                              const bool interrupted) {
  switch (change) {
    case Change::Gain:
      lost_for_good_ = false;
      return interrupted ? Step::RestoreAndEnd : Step::None;
    case Change::Loss:
    case Change::LossTransient:
      if (!session_active) {
        return Step::None; // nothing live to hold
      }
      if (change == Change::Loss) {
        lost_for_good_ = true; // Android sends no Gain after this one
      }
      return interrupted ? Step::None : Step::Begin;
    case Change::LossTransientCanDuck:
    case Change::Unknown:
      return Step::None; // a duck is not a loss: the call is heard over it
  }
  return Step::None;
}

bool AudioFocusInterruption::WantsRetake(const bool session_active) const {
  return lost_for_good_ && session_active;
}

AudioFocusInterruption::Step AudioFocusInterruption::OnRetake(const bool granted, const bool interrupted) {
  if (!granted) {
    return Step::None;
  }
  lost_for_good_ = false;
  return interrupted ? Step::RestoreAndEnd : Step::None;
}

} // namespace pbr
