#pragma once

#include "feature/presenters/AvatarCropFrame.h"

#include <ui/base/Types.h>
#include <ui/paint/Mesh.h>

#include <array>

namespace pbr {

/** Everything here is in the element's own pixels, origin at the top left of its content box. */

/** The picture as one textured quad: `quad_dp` (AvatarCropFrame::Quad, dp from the circle's centre) at `centre_px`. */
ui::Mesh AvatarCropImageMesh(const std::array<AvatarCropFrame::Point, 4>& quad_dp, float px_per_dp,
                             ui::Vector2f centre_px);

/**
 * The dim outside the circle: the area between the circle (a `segments`-gon of radius `radius_px` about `centre_px`)
 * and the rectangle (0,0)-(box_px), as triangles with the vertex colour `shade`. The union of the triangles is exactly
 * that area (the rectangle's corners are vertices), so nothing is covered twice and the circle shows undimmed.
 */
ui::Mesh AvatarCropDimMesh(ui::Vector2f box_px, ui::Vector2f centre_px, float radius_px, int segments,
                           ui::ColourbPremultiplied shade);

/** A ring of width `width_px` just outside the circle, `segments` sides. */
ui::Mesh AvatarCropRingMesh(ui::Vector2f centre_px, float radius_px, float width_px, int segments,
                            ui::ColourbPremultiplied color);

} // namespace pbr
