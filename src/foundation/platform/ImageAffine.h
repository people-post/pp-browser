#pragma once

namespace pbr {

/**
 * An affine map from an output picture's pixel coordinates (u, v) to a source picture's pixel coordinates (x, y):
 *
 *   x = xu * u + xv * v + x0
 *   y = yu * u + yv * v + y0
 *
 * Both sides are continuous pixel coordinates, x right and y down: pixel (i, j) covers [i, i + 1) x [j, j + 1) and its
 * centre is (i + 0.5, j + 0.5). The default is the identity. Plain numbers with no SDL type, so the presenters' crop
 * arithmetic (AvatarCropFrame) can produce it and ImageSampleAffine can consume it.
 */
struct ImageAffine {
  double xu = 1.0;
  double xv = 0.0;
  double x0 = 0.0;
  double yu = 0.0;
  double yv = 1.0;
  double y0 = 0.0;
};

} // namespace pbr
