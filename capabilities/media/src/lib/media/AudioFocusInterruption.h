#pragma once

namespace pbr {

/**
 * Android's audio focus for the live session (the call's focus request), read as AudioSession's interruption
 * (V060): what each focus change, or asking for the focus again, means for it. Pure: AudioSession_Android holds one
 * under its lock and runs the steps.
 *
 * - A loss during a live session (a phone call, another app's call, music or video; not a duck) begins it.
 * - The focus back ends it. The call's audio route is put back first — the audio mode, the communication device and
 *   the speaker choice, which the other call changed — so the handlers reopen the devices on it.
 * - A permanent loss (AUDIOFOCUS_LOSS: another app took the focus for good) gets no focus back from Android. The
 *   app asks for it again when the user is back — the app in front again, a call control pressed — and a granted
 *   request ends the interruption as the focus back would. Until then the call stays on hold (V060).
 */
class AudioFocusInterruption {
public:
  /** AudioManager's focus changes (OnAudioFocusChangeListener). */
  enum class Change { Gain, Loss, LossTransient, LossTransientCanDuck, Unknown };
  enum class Step {
    None,
    /** AudioSession::NotifyInterruptionBegan. */
    Begin,
    /** Put the call's audio route back, then AudioSession::NotifyInterruptionEnded. */
    RestoreAndEnd,
  };

  /** AudioManager's AUDIOFOCUS_* value as a change. */
  static Change FromAndroid(int focus_change);

  Step OnChange(Change change, bool session_active, bool interrupted);
  /** The focus was lost for good during the live session: only asking again brings it back. */
  bool WantsRetake(bool session_active) const;
  /**
   * The answer to asking again. Granted: the focus is back. Not granted: still lost (a delayed grant comes later as
   * a Gain).
   */
  Step OnRetake(bool granted, bool interrupted);
  /** The live session ended: nothing is lost any more. */
  void Reset() { lost_for_good_ = false; }

private:
  bool lost_for_good_ = false;
};

} // namespace pbr
