#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace pbr {

/**
 * How stable this endpoint's network attachment is (call-path-resilience K004) — not physical
 * motion: a still phone on cellular CGNAT rebinds; a laptop roaming between access points churns.
 */
enum class MobilityClass {
  /** Not classified yet, or an old peer without `caps.mobility`. */
  Unknown,
  Stationary,
  Mobile,
};

/** Wire value of `caps.mobility` (K005): "stationary" | "mobile" | "unknown". */
const char* MobilityClassWire(MobilityClass mobility);
/** Unrecognized / missing → Unknown. */
MobilityClass ParseMobilityClass(std::string_view wire);

/**
 * Pinned class from config / CLI: "stationary" | "mobile" | "unknown"; "auto", empty or anything
 * else → nullopt (classify automatically).
 */
std::optional<MobilityClass> ParseMobilityOverride(std::string_view value);
/** Process-wide `--mobility=` (dogfood / hard lab); wins over config when set. */
void SetMobilityCliOverride(std::string value);
std::string MobilityCliOverride();
/** CLI when given, else the config value. */
std::optional<MobilityClass> ResolveMobilityOverride(const std::string& config_value);

/** The primary signals of the current attachment (from the platform network monitor). */
struct MobilityAttachment {
  bool online = false;
  bool cellular = false;
  /** Metered / expensive / constrained — catches hotspot tethering that looks like Wi-Fi. */
  bool expensive = false;
};

/**
 * Classifies this endpoint from its attachment and churn, with hysteresis:
 * - Mobile at once on a cellular or expensive attachment, or after `kMobilityChurnToMobile`
 *   attachment / observed-address changes within `kMobilityChurnWindow`.
 * - Back to Stationary only on a plain attachment once churn has calmed: at most one change in the
 *   window and none for `kMobilityCalmBeforeStationary`.
 * - Offline keeps the class. Unknown until the first attachment is seen. An override pins it.
 * Pure: the caller passes the time; not thread-safe (one owner).
 */
class MobilityClassifier {
public:
  using Clock = std::chrono::steady_clock;

  /** The attachment now; `changed` counts as churn (a network change, not the initial state). */
  void OnAttachment(const MobilityAttachment& attachment, bool changed, Clock::time_point now);
  /** Our observed (public) address moved without a local network change — a NAT rebind. */
  void OnObservedAddressChanged(Clock::time_point now);
  /** Pin the class (config / `--mobility=`); nullopt returns to automatic. */
  void SetOverride(std::optional<MobilityClass> pinned);

  /** Re-evaluates (hysteresis decays with time); returns the class. */
  MobilityClass Evaluate(Clock::time_point now);
  MobilityClass Class() const { return class_; }
  /**
   * When `Evaluate` could next answer differently with no new event: a churn-driven Mobile relaxes
   * once the calm period passes or churn leaves the window. nullopt when only an event can change it.
   */
  std::optional<Clock::time_point> NextReevaluationAt(Clock::time_point now) const;

private:
  void NoteChurn(Clock::time_point now);
  void DropOldChurn(Clock::time_point now);

  std::optional<MobilityClass> override_;
  std::optional<MobilityAttachment> attachment_;
  std::deque<Clock::time_point> churn_;
  MobilityClass class_ = MobilityClass::Unknown;
};

inline constexpr std::chrono::minutes kMobilityChurnWindow{10};
inline constexpr size_t kMobilityChurnToMobile = 3;
inline constexpr std::chrono::minutes kMobilityCalmBeforeStationary{5};
/** Churn signals this close together are one event (a change and its re-probed address). */
inline constexpr std::chrono::seconds kMobilityChurnDedupe{30};

} // namespace pbr
