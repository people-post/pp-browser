#pragma once

#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/ScrollTypes.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

/**
    Implements scrolling behavior that occurs over time.

    Scrolling modes are activated externally, targeting a given element. The actual scrolling takes place during update calls.
 */

class ScrollController {
public:
	enum class Mode {
		None,
		Smoothscroll, // Smooth scrolling to target distance.
		Autoscroll,   // Scrolling with middle mouse button.
		Inertia,      // Applying scrolling inertia when using swipe gesture
		Overscroll,   // Spring-settle after rubber-band overscroll
	};

	void ActivateAutoscroll(Element* target, Vector2i start_position);
	void ActivateSmoothscroll(Element* target, Vector2f delta_distance, ScrollBehavior scroll_behavior);
	void ActivateInertia(Element* target, Vector2f velocity);
	// Settles rubber-banded overscroll back into the valid range. Optional release velocity (px/s).
	void ActivateOverscrollSettle(Element* target, Vector2f release_velocity = {});

	// Instant scroll; when allow_overscroll is true, past-edge deltas rubber-band instead of hard-clamping.
	void InstantScrollOnTarget(Element* target, Vector2f delta_distance, bool allow_overscroll = false);

	bool Update(Vector2i mouse_position, float dp_ratio);

	// Re-apply visual overscroll after layout may have clamped scroll offsets.
	void RestoreOverscrollAfterLayout();

	// Which edges may rubber-band. Used e.g. to block top overscroll while a sheet owns pull-to-dismiss.
	void SetOverscrollEdgesEnabled(bool min_x, bool max_x, bool min_y, bool max_y);
	void ClearPendingOverscroll();

	void IncrementSmoothscrollTarget(Vector2f delta_distance);

	// Resets any active mode and its state.
	void Reset();

	// Sets the scroll behavior for mouse wheel processing and scrollbar interaction.
	void SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor);

	// Returns the autoscroll cursor based on the active scroll velocity.
	String GetAutoscrollCursor(Vector2i mouse_position, float dp_ratio) const;
	// Returns true if autoscroll mode is active and the cursor has been moved outside the idle scroll area.
	bool HasAutoscrollMoved() const;

	Mode GetMode() const { return mode; }
	Element* GetTarget() const { return target; }
	bool HasVisualOverscroll() const;

private:
	// Updates time to now, and returns the delta time since the previous time update.
	float UpdateTime();

	void UpdateAutoscroll(float dt, Vector2i mouse_position, float dp_ratio);

	void UpdateSmoothscroll(float dt, float dp_ratio);

	void UpdateInertia(float dt);

	void UpdateOverscroll(float dt);

	bool HasSmoothscrollReachedTarget() const;

	void PerformScrollOnTarget(Vector2f delta_distance, bool allow_overscroll);

	Vector2f GetScrollOffset() const;
	Vector2f GetScrollRange() const;
	Vector2f GetClientSize() const;
	void SetScrollOffset(Vector2f offset, bool clamp);
	bool AxisScrollable(int axis, float range_axis) const;
	bool EdgeOverscrollAllowed(int axis, float proposed, float range_axis) const;

	Mode mode = Mode::None;

	Element* target = nullptr;
	double previous_update_time = 0;

	Vector2i autoscroll_start_position;
	Vector2f autoscroll_accumulated_length;
	bool autoscroll_moved = false;

	bool smoothscroll_prefer_instant = false;
	float smoothscroll_speed_factor = 1.f;

	Vector2f smoothscroll_target_distance;
	Vector2f smoothscroll_scrolled_distance;
	Vector2f smoothscroll_accumulated_fractional_distance;

	Vector2f inertia_scroll_velocity;
	Vector2f overscroll_velocity;
	// Last unclamped scroll offset applied for rubber-band; restored after layout clamp.
	Vector2f pending_overscroll_offset;
	bool has_pending_overscroll = false;
	// Element holding visual overscroll while finger is down (mode may be None).
	Element* overscroll_element = nullptr;

	bool overscroll_min_x = true;
	bool overscroll_max_x = true;
	bool overscroll_min_y = true;
	bool overscroll_max_y = true;
};

} // namespace Rml
