#include "ScrollController.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"

namespace Rml {

static constexpr float AUTOSCROLL_SPEED_FACTOR = 0.09f;
static constexpr float AUTOSCROLL_DEADZONE = 10.0f;            // [dp]

static constexpr float SMOOTHSCROLL_WINDOW_SIZE = 100.f;       // The window where smoothing is applied, as a distance from scroll start and end. [dp]
static constexpr float SMOOTHSCROLL_MAX_VELOCITY = 10'000.f;   // [dp/s]
static constexpr float SMOOTHSCROLL_VELOCITY_CONSTANT = 800.f; // [dp/s]
static constexpr float SMOOTHSCROLL_VELOCITY_SQUARE_FACTOR = 0.05f;
static constexpr float SMOOTHSCROLL_FIRST_FRAME_DELTA_TIME_MIN = 1.f / 100.f; // To make the scroll feel a bit more responsive. [s]

// Inertia friction: lower = longer coast after a flick. Tuned for mobile touch feel.
static constexpr float INERTIA_FRICTION_FACTOR = 2.4f;
static constexpr float INERTIA_VELOCITY_CUTOFF = 40.0f; // [px/s]
static constexpr float INERTIA_MAX_VELOCITY = 8'000.f;  // [px/s]

// Rubber-band overscroll: asymptotic stretch past edges while dragging / flinging.
static constexpr float OVERSCROLL_STIFFNESS = 0.45f;
static constexpr float OVERSCROLL_SPRING = 28.f;         // baseline spring toward valid range
static constexpr float OVERSCROLL_DAMPING = 10.f;        // velocity damping while settling
static constexpr float OVERSCROLL_STRETCH_GAIN = 3.f;    // extra spring from how far past the edge
static constexpr float OVERSCROLL_SETTLE_EPSILON = 0.5f; // [px]
static constexpr float OVERSCROLL_VELOCITY_EPSILON = 15.f; // [px/s]
// Fraction of fling speed handed to spring settle on edge contact (keep low to avoid "flying").
static constexpr float OVERSCROLL_INERTIA_ABSORB = 0.08f;

// Saturate the delta time to some reasonable FPS value, to avoid large steps in case of stuttering or freezing.
static constexpr float DELTA_TIME_MAX = 1.f / 15.f; // [s]

static float RubberBandDistance(float overscroll, float dimension)
{
	if (overscroll <= 0.f || dimension <= 0.f)
		return 0.f;
	// Asymptotic: stretch grows slower the further past the edge.
	return (overscroll * OVERSCROLL_STIFFNESS * dimension) / (dimension + overscroll * OVERSCROLL_STIFFNESS);
}

static float InvertRubberBandDistance(float visual_overscroll, float dimension)
{
	if (visual_overscroll <= 0.f || dimension <= 0.f)
		return 0.f;
	// Inverse of RubberBandDistance; visual asymptotically approaches stiffness * dimension.
	const float max_visual = dimension * OVERSCROLL_STIFFNESS * 0.99f;
	const float capped = Math::Min(visual_overscroll, max_visual);
	return (capped * dimension) / (OVERSCROLL_STIFFNESS * (dimension - capped));
}

// Map a proposed (unclamped) scroll offset through rubber-banding outside [0, max].
static float ApplyRubberBandAxis(float proposed, float max_scroll, float client_size)
{
	if (proposed < 0.f)
		return -RubberBandDistance(-proposed, client_size);
	if (proposed > max_scroll)
		return max_scroll + RubberBandDistance(proposed - max_scroll, client_size);
	return proposed;
}

// Prefer the dominant fling axis so diagonal release doesn't coast on a tiny secondary range.
static Vector2f LockDominantAxis(Vector2f velocity)
{
	const float ax = Math::Absolute(velocity.x);
	const float ay = Math::Absolute(velocity.y);
	if (ax < 1.f && ay < 1.f)
		return {};
	if (ay >= ax)
		velocity.x = 0.f;
	else
		velocity.y = 0.f;
	return velocity;
}

// Determines the autoscroll velocity based on the distance from the scroll-start mouse position. [px/s]
static Vector2f CalculateAutoscrollVelocity(Vector2f target_delta, float dp_ratio)
{
	target_delta = target_delta / dp_ratio;
	target_delta = {
		Math::Absolute(target_delta.x) < AUTOSCROLL_DEADZONE ? 0.f : target_delta.x,
		Math::Absolute(target_delta.y) < AUTOSCROLL_DEADZONE ? 0.f : target_delta.y,
	};

	// We use a signed square model for the velocity, which seems to work quite well. This is mostly about feeling and tuning.
	return AUTOSCROLL_SPEED_FACTOR * target_delta * Math::Absolute(target_delta);
}

// Determines the smoothscroll velocity based on the distance to the target, and the distance scrolled so far. [px/s]
static Vector2f CalculateSmoothscrollVelocity(Vector2f target_delta, Vector2f scrolled_distance, float dp_ratio)
{
	scrolled_distance = Math::Absolute(scrolled_distance) / dp_ratio;
	target_delta = target_delta / dp_ratio;

	const Vector2f target_delta_abs = Math::Absolute(target_delta);
	Vector2f target_delta_signum = {
		target_delta.x > 0.f ? 1.f : (target_delta.x < 0.f ? -1.f : 0.f),
		target_delta.y > 0.f ? 1.f : (target_delta.y < 0.f ? -1.f : 0.f),
	};

	// The window provides velocity smoothing near the start and end of the scroll.
	const Tween tween(Tween::Exponential, Tween::Out);
	const Vector2f alpha_in = Math::Min(scrolled_distance / SMOOTHSCROLL_WINDOW_SIZE, Vector2f(1.f));
	const Vector2f alpha_out = Math::Min(target_delta_abs / SMOOTHSCROLL_WINDOW_SIZE, Vector2f(1.f));
	const Vector2f smooth_window = {
		0.35f + 0.65f * tween(alpha_in.x) * tween(alpha_out.x),
		0.35f + 0.65f * tween(alpha_in.y) * tween(alpha_out.y),
	};

	const Vector2f velocity_constant = Vector2f(SMOOTHSCROLL_VELOCITY_CONSTANT);
	const Vector2f velocity_square = SMOOTHSCROLL_VELOCITY_SQUARE_FACTOR * target_delta_abs * target_delta_abs;

	// Short scrolls are dominated by the smoothed constant velocity, while the square term is added for quick longer scrolls.
	return dp_ratio * target_delta_signum * smooth_window * Math::Min(velocity_constant + velocity_square, Vector2f(SMOOTHSCROLL_MAX_VELOCITY));
}

void ScrollController::ActivateAutoscroll(Element* in_target, Vector2i start_position)
{
	Reset();
	if (!in_target)
		return;
	target = in_target;
	mode = Mode::Autoscroll;
	autoscroll_start_position = start_position;
	UpdateTime();
}

void ScrollController::ActivateSmoothscroll(Element* in_target, Vector2f delta_distance, ScrollBehavior scroll_behavior)
{
	Reset();
	if (!in_target)
		return;

	target = in_target;

	// Do instant scroll if preferred.
	if (smoothscroll_prefer_instant && scroll_behavior != ScrollBehavior::Smooth)
	{
		PerformScrollOnTarget(delta_distance, false);
		target = nullptr;
		return;
	}

	mode = Mode::Smoothscroll;
	UpdateTime();
	IncrementSmoothscrollTarget(delta_distance);

	// If the target is scrolled to its edge already, simply cancel the smoothscroll operation.
	if (HasSmoothscrollReachedTarget())
		Reset();
}

void ScrollController::ActivateInertia(Element* in_target, Vector2f velocity)
{
	if (!in_target)
	{
		Reset();
		return;
	}

	target = in_target;

	const Vector2f range = GetScrollRange();
	if (!AxisScrollable(0, range.x))
		velocity.x = 0.f;
	if (!AxisScrollable(1, range.y))
		velocity.y = 0.f;
	velocity = LockDominantAxis(velocity);

	if (velocity.x == 0 && velocity.y == 0)
	{
		if (HasVisualOverscroll())
			ActivateOverscrollSettle(in_target, velocity);
		else
			Reset();
		return;
	}

	// Clamp extreme flicks so coast distance stays predictable across devices / HiDPI.
	velocity.x = Math::Clamp(velocity.x, -INERTIA_MAX_VELOCITY, INERTIA_MAX_VELOCITY);
	velocity.y = Math::Clamp(velocity.y, -INERTIA_MAX_VELOCITY, INERTIA_MAX_VELOCITY);

	inertia_scroll_velocity = velocity;
	overscroll_velocity = {};
	mode = Mode::Inertia;
	UpdateTime();

	// Already rubber-banded (released while past an edge): settle with release velocity.
	if (HasVisualOverscroll())
		ActivateOverscrollSettle(in_target, velocity);
}

void ScrollController::ActivateOverscrollSettle(Element* in_target, Vector2f release_velocity)
{
	if (!in_target)
	{
		Reset();
		return;
	}

	target = in_target;

	const Vector2f range = GetScrollRange();
	if (!AxisScrollable(0, range.x))
		release_velocity.x = 0.f;
	if (!AxisScrollable(1, range.y))
		release_velocity.y = 0.f;
	release_velocity = LockDominantAxis(release_velocity);

	// Drop release velocity into edges that aren't allowed to rubber-band.
	if (release_velocity.x < 0.f && !overscroll_min_x)
		release_velocity.x = 0.f;
	if (release_velocity.x > 0.f && !overscroll_max_x)
		release_velocity.x = 0.f;
	if (release_velocity.y < 0.f && !overscroll_min_y)
		release_velocity.y = 0.f;
	if (release_velocity.y > 0.f && !overscroll_max_y)
		release_velocity.y = 0.f;

	overscroll_velocity = release_velocity;
	inertia_scroll_velocity = {};
	mode = Mode::Overscroll;
	UpdateTime();

	if (!HasVisualOverscroll() && Math::Absolute(overscroll_velocity.x) < OVERSCROLL_VELOCITY_EPSILON &&
		Math::Absolute(overscroll_velocity.y) < OVERSCROLL_VELOCITY_EPSILON)
	{
		SetScrollOffset(Math::Clamp(GetScrollOffset(), Vector2f(0.f), GetScrollRange()), true);
		Reset();
	}
}

void ScrollController::InstantScrollOnTarget(Element* in_target, Vector2f delta_distance, bool allow_overscroll)
{
	if (!in_target)
		return;

	// Instant scroll element without changing the current target / mode (except we may leave overscroll).

	Element* safe_target = target;
	Mode safe_mode = mode;
	Vector2f safe_inertia = inertia_scroll_velocity;
	Vector2f safe_overscroll_vel = overscroll_velocity;

	target = in_target;
	PerformScrollOnTarget(delta_distance, allow_overscroll);

	target = safe_target;
	mode = safe_mode;
	inertia_scroll_velocity = safe_inertia;
	overscroll_velocity = safe_overscroll_vel;
}

bool ScrollController::Update(Vector2i mouse_position, float dp_ratio)
{
	const float dt = (mode == Mode::None ? 0.f : UpdateTime());

	switch (mode)
	{
	case Mode::Smoothscroll: UpdateSmoothscroll(dt, dp_ratio); break;
	case Mode::Autoscroll: UpdateAutoscroll(dt, mouse_position, dp_ratio); break;
	case Mode::Inertia: UpdateInertia(dt); break;
	case Mode::Overscroll: UpdateOverscroll(dt); break;
	case Mode::None: break;
	}

	return mode != Mode::None;
}

void ScrollController::RestoreOverscrollAfterLayout()
{
	Element* el = target ? target : overscroll_element;
	if (!has_pending_overscroll || !el)
		return;

	Element* previous_target = target;
	target = el;
	const Vector2f range = GetScrollRange();
	const Vector2f desired = pending_overscroll_offset;
	const bool still_overscrolled = desired.x < -0.5f || desired.y < -0.5f || desired.x > range.x + 0.5f || desired.y > range.y + 0.5f;
	if (!still_overscrolled)
	{
		has_pending_overscroll = false;
		overscroll_element = nullptr;
		target = previous_target;
		return;
	}

	// Layout may have clamped past-edge offsets; re-apply the visual rubber-band for this frame.
	if (AxisScrollable(0, range.x))
		el->SetScrollLeft(desired.x, false);
	if (AxisScrollable(1, range.y))
		el->SetScrollTop(desired.y, false);
	target = previous_target;
}

void ScrollController::UpdateAutoscroll(float dt, Vector2i mouse_position, float dp_ratio)
{
	RMLUI_ASSERT(mode == Mode::Autoscroll && target);

	const Vector2f scroll_delta = Vector2f(mouse_position - autoscroll_start_position);
	const Vector2f scroll_velocity = CalculateAutoscrollVelocity(scroll_delta, dp_ratio);

	autoscroll_accumulated_length += scroll_velocity * dt;

	// Only submit the integer part of the scroll length, accumulate and store fractional parts to enable sub-pixel-per-frame scrolling speeds.
	Vector2f scroll_length_integral;
	autoscroll_accumulated_length.x = Math::DecomposeFractionalIntegral(autoscroll_accumulated_length.x, &scroll_length_integral.x);
	autoscroll_accumulated_length.y = Math::DecomposeFractionalIntegral(autoscroll_accumulated_length.y, &scroll_length_integral.y);

	if (scroll_velocity != Vector2f(0.f))
		autoscroll_moved = true;

	PerformScrollOnTarget(scroll_length_integral, false);
}

void ScrollController::UpdateSmoothscroll(float dt, float dp_ratio)
{
	RMLUI_ASSERT(mode == Mode::Smoothscroll && target);

	const Vector2f target_delta = Vector2f(smoothscroll_target_distance - smoothscroll_scrolled_distance);
	const Vector2f velocity = CalculateSmoothscrollVelocity(target_delta, smoothscroll_scrolled_distance, dp_ratio);

	if (smoothscroll_scrolled_distance == Vector2f{0})
		dt = Math::Max(dt, SMOOTHSCROLL_FIRST_FRAME_DELTA_TIME_MIN);

	Vector2f scroll_distance_fractional = smoothscroll_speed_factor * velocity * dt + smoothscroll_accumulated_fractional_distance;

	Vector2f scroll_distance_integral;
	smoothscroll_accumulated_fractional_distance.x = Math::DecomposeFractionalIntegral(scroll_distance_fractional.x, &scroll_distance_integral.x);
	smoothscroll_accumulated_fractional_distance.y = Math::DecomposeFractionalIntegral(scroll_distance_fractional.y, &scroll_distance_integral.y);

	for (int i = 0; i < 2; i++)
	{
		// Clamp the distance to the target in case of overshooting integration.
		if (target_delta[i] > 0.f)
			scroll_distance_integral[i] = Math::Min(scroll_distance_integral[i], target_delta[i]);
		else if (target_delta[i] < 0.f)
			scroll_distance_integral[i] = Math::Max(scroll_distance_integral[i], target_delta[i]);
		else
			scroll_distance_integral[i] = 0.f;
	}

#if 0
	// Useful debugging output for velocity model tuning.
	Log::Message(Log::LT_INFO, "Scroll  y0 %8.2f   y1 %8.2f    dt %1.4f   v %8.2f   dy %8.2f    frac %1.2f", smoothscroll_scrolled_distance.y,
		target_delta.y, dt, velocity.y, scroll_distance_integral.y, smoothscroll_accumulated_fractional_distance.y);
#endif

	smoothscroll_scrolled_distance += scroll_distance_integral;
	PerformScrollOnTarget(scroll_distance_integral, false);

	if (HasSmoothscrollReachedTarget())
		Reset();
}

void ScrollController::UpdateInertia(float dt)
{
	RMLUI_ASSERT(mode == Mode::Inertia && target);

	if (inertia_scroll_velocity.x == 0.0f && inertia_scroll_velocity.y == 0.0f)
	{
		if (HasVisualOverscroll())
			ActivateOverscrollSettle(target, {});
		else
			Reset();
		return;
	}

	const Vector2f before = GetScrollOffset();
	const Vector2f range = GetScrollRange();

	// Coast with hard clamping so the approach to an edge stays snappy; bounce is settle-only.
	Vector2f scroll_delta = inertia_scroll_velocity * dt;
	PerformScrollOnTarget(scroll_delta, false);

	const Vector2f after = GetScrollOffset();

	float dampening = 1.0f - INERTIA_FRICTION_FACTOR * dt;
	dampening = Math::Max(dampening, 0.f);
	inertia_scroll_velocity *= dampening;

	Vector2f settle_velocity;
	for (int axis = 0; axis < 2; axis++)
	{
		if (!AxisScrollable(axis, range[axis]))
		{
			inertia_scroll_velocity[axis] = 0.f;
			continue;
		}

		const bool hit_min = before[axis] <= 0.5f && after[axis] <= 0.5f && inertia_scroll_velocity[axis] < 0.f;
		const bool hit_max = before[axis] >= range[axis] - 0.5f && after[axis] >= range[axis] - 0.5f && inertia_scroll_velocity[axis] > 0.f;

		if (hit_min || hit_max)
		{
			if (Math::Absolute(inertia_scroll_velocity[axis]) > OVERSCROLL_VELOCITY_EPSILON &&
				EdgeOverscrollAllowed(axis, after[axis] + inertia_scroll_velocity[axis], range[axis]))
			{
				settle_velocity[axis] = inertia_scroll_velocity[axis] * OVERSCROLL_INERTIA_ABSORB;
			}
			inertia_scroll_velocity[axis] = 0.f;
		}
	}

	if (Math::Absolute(inertia_scroll_velocity.x) < INERTIA_VELOCITY_CUTOFF)
		inertia_scroll_velocity.x = 0.0f;
	if (Math::Absolute(inertia_scroll_velocity.y) < INERTIA_VELOCITY_CUTOFF)
		inertia_scroll_velocity.y = 0.0f;

	if (inertia_scroll_velocity.x == 0.f && inertia_scroll_velocity.y == 0.f)
	{
		if (settle_velocity.x != 0.f || settle_velocity.y != 0.f || HasVisualOverscroll())
			ActivateOverscrollSettle(target, settle_velocity);
		else
			Reset();
	}
}

void ScrollController::UpdateOverscroll(float dt)
{
	RMLUI_ASSERT(mode == Mode::Overscroll && target);

	const Vector2f range = GetScrollRange();
	const Vector2f client = GetClientSize();
	Vector2f offset = GetScrollOffset();
	Vector2f target_offset = Math::Clamp(offset, Vector2f(0.f), range);

	for (int axis = 0; axis < 2; axis++)
	{
		if (!AxisScrollable(axis, range[axis]))
		{
			overscroll_velocity[axis] = 0.f;
			offset[axis] = Math::Clamp(offset[axis], 0.f, range[axis]);
			continue;
		}

		const float delta = target_offset[axis] - offset[axis];
		const float stretch = Math::Absolute(delta) / Math::Max(1.f, client[axis]);
		const float spring = OVERSCROLL_SPRING * (1.f + OVERSCROLL_STRETCH_GAIN * stretch);

		overscroll_velocity[axis] += delta * spring * dt;
		overscroll_velocity[axis] *= Math::Max(0.f, 1.f - OVERSCROLL_DAMPING * dt);
		offset[axis] += overscroll_velocity[axis] * dt;

		const float max_over = client[axis] * OVERSCROLL_STIFFNESS;
		if (offset[axis] < -max_over)
		{
			offset[axis] = -max_over;
			overscroll_velocity[axis] = Math::Max(0.f, overscroll_velocity[axis]);
		}
		else if (offset[axis] > range[axis] + max_over)
		{
			offset[axis] = range[axis] + max_over;
			overscroll_velocity[axis] = Math::Min(0.f, overscroll_velocity[axis]);
		}
	}

	SetScrollOffset(offset, false);

	const bool near_rest = Math::Absolute(offset.x - target_offset.x) < OVERSCROLL_SETTLE_EPSILON &&
		Math::Absolute(offset.y - target_offset.y) < OVERSCROLL_SETTLE_EPSILON &&
		Math::Absolute(overscroll_velocity.x) < OVERSCROLL_VELOCITY_EPSILON &&
		Math::Absolute(overscroll_velocity.y) < OVERSCROLL_VELOCITY_EPSILON;

	if (near_rest)
	{
		SetScrollOffset(target_offset, true);
		Reset();
	}
}

bool ScrollController::HasSmoothscrollReachedTarget() const
{
	constexpr float epsilon = 0.1f;
	return (smoothscroll_target_distance - smoothscroll_scrolled_distance).SquaredMagnitude() < epsilon;
}

bool ScrollController::HasVisualOverscroll() const
{
	Element* el = target ? target : overscroll_element;
	if (!el)
		return false;
	const Vector2f offset = {el->GetScrollLeft(), el->GetScrollTop()};
	const Vector2f range = {Math::Max(0.f, el->GetScrollWidth() - el->GetClientWidth()),
		Math::Max(0.f, el->GetScrollHeight() - el->GetClientHeight())};
	constexpr float eps = 0.5f;
	return offset.x < -eps || offset.y < -eps || offset.x > range.x + eps || offset.y > range.y + eps;
}

Vector2f ScrollController::GetScrollOffset() const
{
	RMLUI_ASSERT(target);
	return {target->GetScrollLeft(), target->GetScrollTop()};
}

Vector2f ScrollController::GetScrollRange() const
{
	RMLUI_ASSERT(target);
	return {Math::Max(0.f, target->GetScrollWidth() - target->GetClientWidth()),
		Math::Max(0.f, target->GetScrollHeight() - target->GetClientHeight())};
}

Vector2f ScrollController::GetClientSize() const
{
	RMLUI_ASSERT(target);
	return {target->GetClientWidth(), target->GetClientHeight()};
}

void ScrollController::SetScrollOffset(Vector2f offset, bool clamp)
{
	RMLUI_ASSERT(target);
	const Vector2f range = GetScrollRange();
	if (AxisScrollable(0, range.x))
		target->SetScrollLeft(offset.x, clamp);
	else
		target->SetScrollLeft(Math::Clamp(target->GetScrollLeft(), 0.f, range.x), true);
	if (AxisScrollable(1, range.y))
		target->SetScrollTop(offset.y, clamp);
	else
		target->SetScrollTop(Math::Clamp(target->GetScrollTop(), 0.f, range.y), true);

	if (!clamp)
	{
		pending_overscroll_offset = {target->GetScrollLeft(), target->GetScrollTop()};
		has_pending_overscroll = true;
		overscroll_element = target;
	}
	else
	{
		has_pending_overscroll = false;
		overscroll_element = nullptr;
	}
}

void ScrollController::SetOverscrollEdgesEnabled(bool min_x, bool max_x, bool min_y, bool max_y)
{
	overscroll_min_x = min_x;
	overscroll_max_x = max_x;
	overscroll_min_y = min_y;
	overscroll_max_y = max_y;
}

void ScrollController::ClearPendingOverscroll()
{
	if (has_pending_overscroll)
	{
		Element* el = target ? target : overscroll_element;
		if (el)
		{
			const float max_x = Math::Max(0.f, el->GetScrollWidth() - el->GetClientWidth());
			const float max_y = Math::Max(0.f, el->GetScrollHeight() - el->GetClientHeight());
			el->SetScrollLeft(Math::Clamp(el->GetScrollLeft(), 0.f, max_x), true);
			el->SetScrollTop(Math::Clamp(el->GetScrollTop(), 0.f, max_y), true);
		}
	}
	pending_overscroll_offset = {};
	has_pending_overscroll = false;
	overscroll_element = nullptr;
}

bool ScrollController::AxisScrollable(int axis, float range_axis) const
{
	RMLUI_ASSERT(target);
	auto& computed = target->GetComputedValues();
	const Style::Overflow overflow = (axis == 0) ? computed.overflow_x() : computed.overflow_y();
	if (overflow == Style::Overflow::Scroll)
		return true;
	if (overflow == Style::Overflow::Auto)
		return range_axis > 0.5f;
	return false;
}

bool ScrollController::EdgeOverscrollAllowed(int axis, float proposed, float range_axis) const
{
	if (axis == 0)
	{
		if (proposed < 0.f)
			return overscroll_min_x;
		if (proposed > range_axis)
			return overscroll_max_x;
	}
	else
	{
		if (proposed < 0.f)
			return overscroll_min_y;
		if (proposed > range_axis)
			return overscroll_max_y;
	}
	return true;
}

void ScrollController::PerformScrollOnTarget(Vector2f delta_distance, bool allow_overscroll)
{
	RMLUI_ASSERT(target);

	const Vector2f range = GetScrollRange();
	const Vector2f client = GetClientSize();
	Vector2f offset = GetScrollOffset();
	bool moved = false;

	for (int axis = 0; axis < 2; axis++)
	{
		const float delta = delta_distance[axis];
		if (delta == 0.f || !AxisScrollable(axis, range[axis]))
			continue;

		float proposed = offset[axis] + delta;
		if (allow_overscroll)
		{
			if (offset[axis] < 0.f)
				proposed = -InvertRubberBandDistance(-offset[axis], client[axis]) + delta;
			else if (offset[axis] > range[axis])
				proposed = range[axis] + InvertRubberBandDistance(offset[axis] - range[axis], client[axis]) + delta;

			if (!EdgeOverscrollAllowed(axis, proposed, range[axis]))
				proposed = Math::Clamp(proposed, 0.f, range[axis]);
			else
				proposed = ApplyRubberBandAxis(proposed, range[axis], client[axis]);
		}
		else
		{
			proposed = Math::Clamp(proposed, 0.f, range[axis]);
		}

		offset[axis] = proposed;
		moved = true;
	}

	if (!moved)
		return;

	const bool visually_overscrolled =
		offset.x < -0.5f || offset.y < -0.5f || offset.x > range.x + 0.5f || offset.y > range.y + 0.5f;
	const bool clamp = !(allow_overscroll && visually_overscrolled);

	if (AxisScrollable(0, range.x))
		target->SetScrollLeft(offset.x, clamp);
	if (AxisScrollable(1, range.y))
		target->SetScrollTop(offset.y, clamp);

	if (!clamp)
	{
		pending_overscroll_offset = {target->GetScrollLeft(), target->GetScrollTop()};
		has_pending_overscroll = true;
		overscroll_element = target;
	}
	else if (allow_overscroll)
	{
		has_pending_overscroll = false;
		overscroll_element = nullptr;
	}
}

void ScrollController::IncrementSmoothscrollTarget(Vector2f delta_distance)
{
	auto OppositeDirection = [](float a, float b) { return (a < 0.f && b > 0.f) || (a > 0.f && b < 0.f); };
	Vector2f delta = smoothscroll_target_distance - smoothscroll_scrolled_distance;

	// Reset movement state if we start scrolling in the opposite direction.
	for (int i = 0; i < 2; i++)
	{
		if (OppositeDirection(delta_distance[i], delta[i]))
		{
			smoothscroll_target_distance[i] = 0.f;
			smoothscroll_scrolled_distance[i] = 0.f;
			smoothscroll_accumulated_fractional_distance[i] = 0.f;
		}
	}

	// Clamp the delta distance to the scrollable area.
	const Vector2f scroll_offset = {target->GetScrollLeft(), target->GetScrollTop()};
	const Vector2f max_offset = {target->GetScrollWidth() - target->GetClientWidth(), target->GetScrollHeight() - target->GetClientHeight()};

	const Vector2f target_offset = scroll_offset + smoothscroll_target_distance - smoothscroll_scrolled_distance;
	const Vector2f clamped_delta = Math::Clamp(delta_distance + target_offset, Vector2f(0.f), max_offset) - target_offset;

	smoothscroll_target_distance += clamped_delta;
}

void ScrollController::Reset()
{
	Element* el = target ? target : overscroll_element;
	if (el && has_pending_overscroll)
	{
		const float max_x = Math::Max(0.f, el->GetScrollWidth() - el->GetClientWidth());
		const float max_y = Math::Max(0.f, el->GetScrollHeight() - el->GetClientHeight());
		el->SetScrollLeft(Math::Clamp(el->GetScrollLeft(), 0.f, max_x), true);
		el->SetScrollTop(Math::Clamp(el->GetScrollTop(), 0.f, max_y), true);
	}

	mode = Mode::None;
	target = nullptr;

	autoscroll_start_position = Vector2i{};
	autoscroll_accumulated_length = Vector2f{};
	autoscroll_moved = false;

	smoothscroll_target_distance = Vector2f{};
	smoothscroll_scrolled_distance = Vector2f{};
	smoothscroll_accumulated_fractional_distance = Vector2f{};
	// Keep smoothscroll configuration parameters.

	inertia_scroll_velocity = Vector2f{};
	overscroll_velocity = Vector2f{};
	pending_overscroll_offset = Vector2f{};
	has_pending_overscroll = false;
	overscroll_element = nullptr;
}

void ScrollController::SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor)
{
	smoothscroll_prefer_instant = (scroll_behavior == ScrollBehavior::Instant);
	smoothscroll_speed_factor = speed_factor;
}

String ScrollController::GetAutoscrollCursor(Vector2i mouse_position, float dp_ratio) const
{
	RMLUI_ASSERT(mode == Mode::Autoscroll);

	const Vector2f scroll_delta = Vector2f(mouse_position - autoscroll_start_position);
	const Vector2f scroll_velocity = CalculateAutoscrollVelocity(scroll_delta, dp_ratio);

	if (scroll_velocity == Vector2f(0.f))
		return "rmlui-scroll-idle";

	String result = "rmlui-scroll";

	if (scroll_velocity.y > 0.f)
		result += "-up";
	else if (scroll_velocity.y < 0.f)
		result += "-down";

	if (scroll_velocity.x > 0.f)
		result += "-right";
	else if (scroll_velocity.x < 0.f)
		result += "-left";

	return result;
}

bool ScrollController::HasAutoscrollMoved() const
{
	return mode == Mode::Autoscroll && autoscroll_moved;
}

float ScrollController::UpdateTime()
{
	const double previous_tick = previous_update_time;
	previous_update_time = GetSystemInterface()->GetElapsedTime();

	const float dt = float(previous_update_time - previous_tick);
	return Math::Min(dt, DELTA_TIME_MAX);
}

} // namespace Rml
