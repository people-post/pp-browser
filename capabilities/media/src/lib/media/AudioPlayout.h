#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace pbr {

/** Shortest packet the jitter buffer counts in (ms): the frame counts below are its, and longer packets divide them. */
inline constexpr int kAudioMinPacketMs = 20;
/** Longest Opus packet (120 ms). */
inline constexpr int kAudioMaxPacketMs = 120;

/** `ms` in packets of `packet_ms`, rounded up, at least 1. */
inline size_t AudioPacketsFor(int ms, int packet_ms) {
  return static_cast<size_t>(std::max(1, (ms + packet_ms - 1) / packet_ms));
}

/**
 * Arrival-jitter estimator for one publisher (adaptive jitter target, DEV/PLAN-adaptive-jitter.md §1).
 * Lateness = relative delay (recv_ms − capture time) above its 5 s sliding minimum, bucketed per packet in a
 * forgetting histogram (~10 s memory). Target = 99th-percentile bucket + 1 packet, clamped to 60..300 ms
 * (3..15 packets of 20 ms, 2..7 of 40 ms). Rises as soon as late packets arrive; decays as old samples fade. Its top
 * stays below the buffer's 400 ms cap (V065's budget), so above the target there is room to catch up in silences.
 * The capture time is the sender's (V066): a sender that pauses sends nothing and its clock moves on, so a pause is
 * no lateness; the minimum follows a clock drift between the two ends.
 *
 * An outage is not jitter (#593: the stream stopped for 1.2 s, then its backlog came in a burst; the target pinned at
 * its top for the rest of the call). A packet later than the target's top could ever hold starts an outage, and the
 * backlog — every packet still later than the target was before it — is left out: no target could have played it in
 * time, so it is concealed, never waited for.
 */
class AudioArrivalJitter {
public:
  static constexpr int kMinTargetMs = 60;
  static constexpr int kMaxTargetMs = 300;
  /** At 20 ms packets. */
  static constexpr size_t kMinTargetFrames = kMinTargetMs / kAudioMinPacketMs;
  static constexpr size_t kMaxTargetFrames = kMaxTargetMs / kAudioMinPacketMs;
  static constexpr size_t kBaselinePackets = 250;
  static constexpr double kForget = 0.998;
  static constexpr double kQuantile = 0.99;

  /** The publisher's packet length (ms, kAudioMinPacketMs..kAudioMaxPacketMs); a change starts over. */
  void SetPacketMs(int packet_ms) {
    packet_ms = std::clamp(packet_ms, kAudioMinPacketMs, kAudioMaxPacketMs);
    if (packet_ms != packet_ms_) {
      Reset();
      packet_ms_ = packet_ms;
    }
  }
  int PacketMs() const { return packet_ms_; }
  size_t MinTargetFrames() const { return AudioPacketsFor(kMinTargetMs, packet_ms_); }
  size_t MaxTargetFrames() const { return static_cast<size_t>(kMaxTargetMs / packet_ms_); }

  /** A packet captured at `capture_ms` (the sender's clock, unwrapped) came at `recv_ms` (ours, monotonic). */
  void OnArrival(int64_t capture_ms, int64_t recv_ms) {
    const int64_t late_ms = Lateness(recv_ms - capture_ms);
    const int64_t held_ms = static_cast<int64_t>(TargetFrames() + 1) * packet_ms_;
    if (late_ms > kMaxTargetMs) {
      outage_ = true;
    }
    if (outage_ && late_ms <= held_ms) {
      outage_ = false; // on time again: what is older than this packet and late is still the backlog
      backlog_before_ms_ = capture_ms;
    }
    if (late_ms > held_ms && (outage_ || capture_ms < backlog_before_ms_)) {
      ++outage_arrivals_;
      return; // the outage's backlog
    }
    const size_t bucket = std::min<size_t>(static_cast<size_t>(late_ms / packet_ms_), MaxTargetFrames());
    for (double& p : hist_) {
      p *= kForget;
    }
    hist_[bucket] += 1.0 - kForget;
    mass_ = mass_ * kForget + (1.0 - kForget);
  }

  size_t TargetFrames() const {
    if (mass_ <= 0.0) {
      return MinTargetFrames();
    }
    double acc = 0.0;
    for (size_t k = 0; k < hist_.size(); ++k) {
      acc += hist_[k];
      if (acc >= kQuantile * mass_) {
        return std::clamp(k + 1, MinTargetFrames(), MaxTargetFrames());
      }
    }
    return MaxTargetFrames();
  }

  /** Packets left out of the estimate as an outage's backlog. */
  uint64_t OutageArrivals() const { return outage_arrivals_; }

  void Reset() {
    hist_.fill(0.0);
    mass_ = 0.0;
    arrivals_ = 0;
    mins_.clear();
    outage_ = false;
    backlog_before_ms_ = INT64_MIN;
  }

private:
  /** Relative delay `d` above the sliding minimum of the last kBaselinePackets. */
  int64_t Lateness(const int64_t d) {
    const uint64_t idx = arrivals_++;
    while (!mins_.empty() && mins_.back().second >= d) {
      mins_.pop_back();
    }
    mins_.emplace_back(idx, d);
    while (mins_.front().first + kBaselinePackets <= idx) {
      mins_.pop_front();
    }
    return d - mins_.front().second;
  }

  std::array<double, kMaxTargetFrames + 1> hist_{};
  int packet_ms_ = kAudioMinPacketMs;
  double mass_ = 0.0;
  uint64_t arrivals_ = 0;
  bool outage_ = false;
  /** The first on-time packet after an outage: late packets captured before it are that outage's backlog too. */
  int64_t backlog_before_ms_ = INT64_MIN;
  uint64_t outage_arrivals_ = 0;
  std::deque<std::pair<uint64_t, int64_t>> mins_; // (arrival index, d) — monotonic sliding minimum
};

/** One received Opus packet (channel 0) for the jitter buffer. */
struct AudioPacket {
  /** Orders packets and shows losses (FEC covers the one right before the next packet); never a clock. */
  uint32_t seq = 0;
  /**
   * When the packet's first sample was captured (MediaDataFrame::capture_ts): ms on the sender's clock, wrapping at
   * 2^32. Playout schedules by it (V066).
   */
  uint32_t capture_ts = 0;
  /** The sender session (MediaDataFrame::instance), whose clock the capture times are on; another one restarts. */
  uint64_t instance = 0;
  /** Monotonic (steady_clock) receive time in ms — not wall clock, so an NTP step can't look
   *  like packet lateness to the arrival-jitter estimator (M6). */
  int64_t recv_ms = 0;
  /** Audio the packet holds (ms, from its Opus TOC); 0 = kAudioMinPacketMs. */
  int duration_ms = 0;
  std::vector<uint8_t> payload;
};

/** Result of one playout pop. */
struct AudioPlayoutPop {
  enum class Kind { Empty, Packet, Gap };
  Kind kind = Kind::Empty;
  /** Packet: this packet's seq. Gap: the missing seq. */
  uint32_t seq = 0;
  /** Packet: its bytes. Gap: the NEXT queued packet's bytes (Opus in-band FEC source). */
  std::vector<uint8_t> payload;
  /** Gap only: `payload` is the packet right after the missing seq, so its LBRR covers it. */
  bool fec_usable = false;
};

/**
 * Per-publisher packet jitter buffer (receiver only; hop stays blind), scheduled by the sender's capture time (V066).
 *
 * The playout head is a position on the sender's clock: the capture time of the next sample to play. It starts at the
 * first packet once the minimum target (60 ms) is queued and moves on by what each 20 ms slot plays or conceals, so a
 * packet captured at t plays about target after a packet captured at t arrives — the anchor, plus the target, plus t.
 * A packet whose whole span the head has passed is a late drop. A gap in capture time with no seq missing is a sender
 * pause: silence, played as Empty slots (not an underrun) and, above the target, skipped down to it — so the packet
 * that ends a pause plays on time however long the pause was (#605). A missing seq is a loss: a Gap slot (FEC from
 * the next packet when that one is queued), or skipped when the buffer is deep or the hole wide. Seq orders packets,
 * shows losses and duplicates; it is never a clock.
 *
 * An underrun (nothing queued) waits for the stream while the delay stays within the target's top (WaitBudgetMs,
 * V065's budget): a late packet is played, the schedule moves later by the wait. Past the budget the stream is in an
 * outage or a pause: the head goes to where the time is and moves on with it — the stale backlog that ends an outage
 * is late (concealed, never waited for, #593), the packet that ends a pause is on time.
 *
 * Clock drift (the two ends' clocks differ by tens of ppm) needs no estimator: a slower receiver queues more, shed by
 * the engine skipping decoded silence above the target; a faster one runs dry, and the underrun's wait moves the
 * schedule later by what it waited.
 *
 * A sender restart is a new `instance` (or, without one, a clock far behind the head): the buffer starts over. A stream
 * that stays behind the head for a second — every packet late, none on time — is re-timed, not a backlog: the buffer
 * starts over on it, so no stream is late forever.
 *
 * Playout holds at most kMaxDelayMs (400 ms, V065's budget). Delays are in ms, counted in the publisher's packets,
 * whose length the first packet tells (20 ms → 3..15 / cap 20, 40 ms → 2..7 / cap 10); the `k*Frames` constants are
 * the 20 ms counts. Packets are decoded at pop time so a missing seq can be recovered from the next packet's FEC data.
 *
 * Catch-up is never heard (V065): above the target the engine skips decoded silence (NoteSilenceDrop); over the cap
 * it skips silence first, as much as the excess, and only then cuts speech to the cap (CutToCap, oldest first). Push
 * itself cuts only past kHardMaxDelayMs of queued packets: a burst while nothing is popped (a device pause), bounded memory.
 */
class AudioJitterBuffer {
public:
  static constexpr int kFrameMs = kAudioMinPacketMs;
  static constexpr int kTargetDelayMs = AudioArrivalJitter::kMinTargetMs;
  /** The most audio playout holds (V065: ~300 ms mouth to ear, 400 ms at most). */
  static constexpr int kMaxDelayMs = 400;
  static_assert(AudioArrivalJitter::kMaxTargetMs < kMaxDelayMs, "the cap leaves room above the target");
  /** What a burst may queue between pops (nothing popped: a device pause) before Push cuts it. */
  static constexpr int kHardMaxDelayMs = 2 * kMaxDelayMs;
  static constexpr size_t kTargetFrames = AudioArrivalJitter::kMinTargetFrames; // kept: minimum/steady target
  static constexpr size_t kMaxFrames = kMaxDelayMs / kFrameMs;
  static constexpr size_t kHardMaxFrames = kHardMaxDelayMs / kFrameMs;
  /** The pre-adaptive 200 ms cap: congestion signal for bitrate adaptation. */
  static constexpr int kPressureFullMs = 200;
  /** A capture time this far behind the head is a sender that started over (a stream without an instance). */
  static constexpr int64_t kRestartBehindMs = 2000;
  /** Every packet late for this long (receive time), none on time: the stream is re-timed, not a backlog. */
  static constexpr int64_t kStaleRunMs = 1000;

  void Push(AudioPacket packet) {
    if (packet.payload.empty()) {
      return;
    }
    const int packet_ms =
        std::clamp(packet.duration_ms > 0 ? packet.duration_ms : kFrameMs, kAudioMinPacketMs, kAudioMaxPacketMs);
    if (packet_ms != PacketMs()) {
      arrival_.SetPacketMs(packet_ms);
      Reset(); // another packet length: a restarted sender (another build) — start over in its packets
    }
    if (AnotherSender(packet)) {
      Reset(); // a restarted sender (re-StartDuplex / BeginSession / a new 1:1 numbering): its clock starts over
    }
    const int64_t at = Unwrap(packet);
    if (primed_ && at + PacketMs() <= head_) {
      if (!StaleRun(packet.recv_ms)) {
        arrival_.OnArrival(at, packet.recv_ms);
        ++drops_late_;
        return;
      }
      Restart(); // behind the head for a second: re-timed, start over on it
    }
    late_since_ms_.reset();
    Insert(Held{at, std::move(packet)});
  }

  size_t size() const { return queue_.size(); }
  /** The publisher's packet length (ms): what one queued packet, a Gap, and a target frame stand for. */
  int PacketMs() const { return arrival_.PacketMs(); }
  uint64_t drops_overflow() const { return drops_overflow_; }
  uint64_t drops_late() const { return drops_late_; }
  uint64_t underruns() const { return underruns_; }
  uint64_t gaps() const { return gaps_; }
  /** Slots played as a sender's pause (silence, not an underrun). */
  uint64_t pause_slots() const { return pause_slots_; }
  /** Adaptive target from the arrival-jitter estimator (AudioArrivalJitter), 60..400 ms in packets. */
  size_t TargetFrames() const { return arrival_.TargetFrames(); }
  int64_t TargetMs() const { return static_cast<int64_t>(TargetFrames()) * PacketMs(); }
  /**
   * Audio between the playout head and the end of the newest queued packet (ms, on the sender's clock): the delay a
   * packet arriving now is played with. A pause inside it counts (the head has to pass it).
   */
  int64_t DepthMs() const {
    if (queue_.empty()) {
      return 0;
    }
    const int64_t from = primed_ ? head_ : queue_.front().at;
    return std::max<int64_t>(0, queue_.back().at + PacketMs() - from);
  }
  /** True once the buffer holds more than one packet of slack above target. */
  bool OverTarget() const { return DepthMs() > TargetMs() + PacketMs(); }
  /** The cap in the publisher's packets (kMaxDelayMs). */
  size_t CapFrames() const { return static_cast<size_t>(kMaxDelayMs / PacketMs()); }
  /** Packets' worth past the cap (0 within it): what catch-up has to shed, silence first. */
  size_t ExcessOverCap() const {
    const int64_t excess = DepthMs() - kMaxDelayMs;
    return excess > 0 ? static_cast<size_t>((excess + PacketMs() - 1) / PacketMs()) : 0;
  }
  /** Over the cap with no silence left to skip: speech goes, oldest first (counted as speech drops). */
  void CutToCap() { CutTo(kMaxDelayMs); }
  /** Engine dropped a decoded silent frame to catch up (known-silence, not counted as speech). */
  void NoteSilenceDrop() { ++drops_silence_; }
  uint64_t silence_drops() const { return drops_silence_; }
  /** Packets cut at the cap or the hard bound: content unknown, so counted as speech. */
  uint64_t speech_drops() const { return drops_overflow_; }

  /**
   * How long an underrun waits for the stream (ms) before it is an outage or a pause: up to the target's top, so what
   * a wait lets in still fits under the cap and is shed in silences, not cut.
   */
  int WaitBudgetMs() const { return std::max(0, AudioArrivalJitter::kMaxTargetMs - static_cast<int>(TargetMs())); }

  /**
   * One 20 ms slot's playout. Before the minimum target was queued once → Empty (no underrun). Then: nothing queued →
   * Empty + underrun; a sender's pause in front → Empty (silence; skipped down to the target when above it); the next
   * packet due → Packet; a missing seq before it → Gap (`fec_usable` only for the seq right before it, whose LBRR it
   * carries; the front stays queued, its bytes returned), or the hole skipped when the buffer already holds more than
   * the target or the hole is wider than it.
   */
  AudioPlayoutPop PopForPlayout() {
    if (!primed_ && !Prime()) {
      return {};
    }
    if (queue_.empty()) {
      return Underrun();
    }
    waited_ms_ = 0;
    ShortenPauseAboveTarget();
    if (PauseAheadMs() >= kAudioMinPacketMs) {
      return PauseSlot();
    }
    if (AheadMs() < kAudioMinPacketMs || SkipHole()) {
      return PlayFront();
    }
    return GapSlot();
  }

  void Reset() {
    Restart();
    instance_.reset();
    arrival_.Reset();
  }

  /**
   * 0 = healthy, 1 = severe (underruns dominate). The "full" reference tracks the adaptive target
   * so a buffer sitting at its own target+1 (e.g. held there by silence catch-up) doesn't read as
   * congested; at the minimum target (60 ms) this is the original fixed 200 ms reference (I1).
   * `window_pops` counts 20 ms playout slots, as underruns do.
   */
  double Pressure(uint64_t window_pops) const {
    const size_t full = TargetFrames() + AudioPacketsFor(kPressureFullMs - kTargetDelayMs, PacketMs());
    if (window_pops == 0) {
      return queue_.size() >= full ? 1.0 : 0.0;
    }
    const double u = static_cast<double>(underruns_) / static_cast<double>(window_pops);
    const double fill = static_cast<double>(queue_.size()) / static_cast<double>(full);
    return std::min(1.0, std::max(u * 2.0, fill > 0.9 ? fill : 0.0));
  }

private:
  /** A queued packet and its capture time, unwrapped onto the buffer's 64-bit timeline. */
  struct Held {
    int64_t at = 0;
    AudioPacket packet;
  };

  /**
   * Another sender session than the queued one: another instance, or — a stream without one (instance 0) — a clock
   * far behind the head. Under one instance a packet far behind is a straggler: its sender never started over.
   */
  bool AnotherSender(const AudioPacket& packet) const {
    if (!instance_) {
      return false;
    }
    if (packet.instance != *instance_) {
      return true;
    }
    return *instance_ == 0 && primed_ && head_ - Unwrap(packet) > kRestartBehindMs;
  }

  /** The packet's capture time on the 64-bit timeline: the nearest to the last one seen, modulo 2^32 (wrap). */
  int64_t Unwrap(const AudioPacket& packet) const {
    if (!instance_) {
      return packet.capture_ts;
    }
    return last_at_ + static_cast<int32_t>(packet.capture_ts - static_cast<uint32_t>(last_at_));
  }

  /** A late packet: true once late ones alone have kept coming for kStaleRunMs. */
  bool StaleRun(const int64_t recv_ms) {
    if (!late_since_ms_) {
      late_since_ms_ = recv_ms;
    }
    return recv_ms - *late_since_ms_ >= kStaleRunMs;
  }

  void Insert(Held held) {
    if (!instance_) {
      instance_ = held.packet.instance;
    }
    last_at_ = held.at;
    auto it = queue_.begin();
    while (it != queue_.end() && it->packet.seq < held.packet.seq) {
      ++it;
    }
    if (it != queue_.end() && it->packet.seq == held.packet.seq) {
      return; // duplicate — not lateness, don't feed the estimator (M4)
    }
    arrival_.OnArrival(held.at, held.packet.recv_ms);
    queue_.insert(it, std::move(held));
    // Memory, not delay: what a burst queued while nothing was popped. A packet far ahead of the head (a pause, a
    // sender that started over) is one packet, never a reason to cut.
    DropOldestWhile([&] { return queue_.size() > static_cast<size_t>(kHardMaxDelayMs / PacketMs()); });
  }

  /**
   * Prime at the minimum target (60 ms), not the adaptive one: a single early outlier can otherwise skew
   * TargetFrames() (tiny sample count) and delay first audio for no reason (M3). The adaptive target keeps governing
   * hole-skip, OverTarget and catch-up.
   */
  bool Prime() {
    if (queue_.size() < arrival_.MinTargetFrames()) {
      return false;
    }
    primed_ = true;
    head_ = queue_.front().at;
    next_seq_ = queue_.front().packet.seq;
    return true;
  }

  /** Nothing queued: wait within the budget (the schedule moves later), then the head moves on with the time. */
  AudioPlayoutPop Underrun() {
    ++underruns_;
    if (waited_ms_ == 0) {
      dry_head_ = head_;
    }
    waited_ms_ += kAudioMinPacketMs;
    if (waited_ms_ > WaitBudgetMs()) {
      head_ = std::max(head_, dry_head_ + waited_ms_); // where the time is: the wait is undone, never kept
    }
    return {};
  }

  /** Time between the head and the front packet (ms; < 0 when the front's span has begun). */
  int64_t AheadMs() const { return queue_.front().at - head_; }
  /** Seqs missing before the front: lost packets, whose time is part of AheadMs. */
  uint32_t MissingAhead() const {
    const uint32_t front = queue_.front().packet.seq;
    return front > next_seq_ ? front - next_seq_ : 0;
  }
  /** Of the time before the front, what no lost packet accounts for: the sender sent nothing (a pause). */
  int64_t PauseAheadMs() const { return AheadMs() - static_cast<int64_t>(MissingAhead()) * PacketMs(); }

  /** A pause in front is silence: above the target, skip it down to the target (catch-up never heard, V065). */
  void ShortenPauseAboveTarget() {
    const int64_t pause = PauseAheadMs();
    const int64_t over = DepthMs() - TargetMs();
    if (pause > 0 && over > 0) {
      head_ += std::min(pause, over);
    }
  }

  AudioPlayoutPop PauseSlot() {
    head_ += kAudioMinPacketMs;
    ++pause_slots_;
    return {};
  }

  /** A hole of lost packets is skipped (front played) when the buffer is deep or the hole wider than the target. */
  bool SkipHole() const { return queue_.size() > TargetFrames() || MissingAhead() > TargetFrames(); }

  AudioPlayoutPop PlayFront() {
    Held& front = queue_.front();
    AudioPlayoutPop out;
    out.kind = AudioPlayoutPop::Kind::Packet;
    out.seq = front.packet.seq;
    out.payload = std::move(front.packet.payload);
    head_ = front.at + PacketMs();
    next_seq_ = front.packet.seq + 1;
    queue_.pop_front();
    return out;
  }

  /** Conceal one missing seq; the front stays queued and lends its bytes (its LBRR covers the seq right before it). */
  AudioPlayoutPop GapSlot() {
    const AudioPacket& front = queue_.front().packet;
    AudioPlayoutPop out;
    out.kind = AudioPlayoutPop::Kind::Gap;
    out.seq = next_seq_;
    out.payload = front.payload;
    out.fec_usable = front.seq == next_seq_ + 1;
    ++gaps_;
    ++next_seq_;
    head_ += PacketMs();
    return out;
  }

  /** Drop the oldest packets until the depth is within `ms`; their time can never play: the head moves past it. */
  void CutTo(const int64_t ms) {
    DropOldestWhile([&] { return DepthMs() > ms; });
  }

  /** Drop the oldest packets while `more` holds, counted as speech drops; the head moves past their time. */
  template <typename More>
  void DropOldestWhile(const More& more) {
    while (!queue_.empty() && more()) {
      const Held& oldest = queue_.front();
      if (primed_) {
        head_ = std::max(head_, oldest.at + PacketMs());
        next_seq_ = std::max(next_seq_, oldest.packet.seq + 1);
      }
      queue_.pop_front();
      ++drops_overflow_;
    }
  }

  /** Start the playout over on the stream (the estimator and the sender kept). */
  void Restart() {
    queue_.clear();
    primed_ = false;
    head_ = 0;
    next_seq_ = 0;
    waited_ms_ = 0;
    dry_head_ = 0;
    late_since_ms_.reset();
  }

  AudioArrivalJitter arrival_;
  std::deque<Held> queue_;
  /** The sender session the queued packets are from (set by the first packet; another one starts over). */
  std::optional<uint64_t> instance_;
  /** The last queued packet's capture time: the reference that unwraps the next one. */
  int64_t last_at_ = 0;
  bool primed_ = false;
  /** The playout head: the capture time (64-bit timeline) of the next sample to play. */
  int64_t head_ = 0;
  /** The seq after the last one played or concealed: a front past it has lost packets before it. */
  uint32_t next_seq_ = 0;
  /** The current underrun: how long it has waited, and where the head stood when the queue ran dry. */
  int waited_ms_ = 0;
  int64_t dry_head_ = 0;
  /** When the current run of late packets (none on time between) began, by receive time. */
  std::optional<int64_t> late_since_ms_;
  uint64_t drops_overflow_ = 0;
  uint64_t drops_late_ = 0;
  uint64_t drops_silence_ = 0;
  uint64_t underruns_ = 0;
  uint64_t gaps_ = 0;
  uint64_t pause_slots_ = 0;
};

/** Peak below 1 % of full scale (≈ −40 dBFS): safe to skip when catching up (adaptive jitter §2). */
constexpr int kCatchUpSilencePeak = 328; // 0.01 × 32768
inline bool IsCatchUpSilence(const std::vector<int16_t>& pcm, size_t samples) {
  const size_t n = std::min(samples, pcm.size());
  for (size_t i = 0; i < n; ++i) {
    if (pcm[i] >= kCatchUpSilencePeak || pcm[i] <= -kCatchUpSilencePeak) {
      return false;
    }
  }
  return true;
}

/** Saturating mix of mono s16 frames into `out` (size = samples). */
inline void MixPcmSat(std::vector<int16_t>& out, const std::vector<int16_t>& in) {
  const size_t n = std::min(out.size(), in.size());
  for (size_t i = 0; i < n; ++i) {
    const int sum = static_cast<int>(out[i]) + static_cast<int>(in[i]);
    out[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, sum)));
  }
}

/**
 * Playout gain with a soft knee: linear up to 70 % of full scale, then tanh-compressed so a
 * boosted peak approaches but never clips full scale (no hard-clip distortion).
 */
inline void ApplySoftGain(std::vector<int16_t>& pcm, float gain) {
  constexpr float kKnee = 0.7f;
  for (auto& s : pcm) {
    float y = static_cast<float>(s) / 32768.f * gain;
    const float a = std::fabs(y);
    if (a > kKnee) {
      y = std::copysign(kKnee + (1.f - kKnee) * std::tanh((a - kKnee) / (1.f - kKnee)), y);
    }
    s = static_cast<int16_t>(std::lrint(std::clamp(y * 32768.f, -32768.f, 32767.f)));
  }
}

} // namespace pbr
