#include "gui/avatar/AvatarCropMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pbr {
namespace {

constexpr double kTwoPi = 6.283185307179586;

ui::Vertex Vertex(const ui::Vector2f position, const ui::ColourbPremultiplied colour,
                  const ui::Vector2f tex = {0.0f, 0.0f}) {
  ui::Vertex v;
  v.position = position;
  v.colour = colour;
  v.tex_coord = tex;
  return v;
}

void Triangle(ui::Mesh& mesh, const int a, const int b, const int c) {
  mesh.indices.push_back(a);
  mesh.indices.push_back(b);
  mesh.indices.push_back(c);
}

ui::Vector2f OnCircle(const ui::Vector2f centre, const float radius, const double angle) {
  return {centre.x + radius * static_cast<float>(std::cos(angle)),
          centre.y + radius * static_cast<float>(std::sin(angle))};
}

// Where the ray from `centre` at `angle` leaves the rectangle (0,0)-(box).
ui::Vector2f OnRectangle(const ui::Vector2f box, const ui::Vector2f centre, const double angle) {
  const double dx = std::cos(angle);
  const double dy = std::sin(angle);
  const double inf = std::numeric_limits<double>::infinity();
  const double tx = dx > 1e-12 ? (box.x - centre.x) / dx : dx < -1e-12 ? -centre.x / dx : inf;
  const double ty = dy > 1e-12 ? (box.y - centre.y) / dy : dy < -1e-12 ? -centre.y / dy : inf;
  const double t = std::min(tx, ty);
  return {centre.x + static_cast<float>(dx * t), centre.y + static_cast<float>(dy * t)};
}

double NormalisedAngle(const double a) {
  double r = std::fmod(a, kTwoPi);
  if (r < 0.0) {
    r += kTwoPi;
  }
  return r;
}

} // namespace

ui::Mesh AvatarCropImageMesh(const std::array<AvatarCropFrame::Point, 4>& quad_dp, const float px_per_dp,
                             const ui::Vector2f centre_px) {
  ui::Mesh mesh;
  const ui::ColourbPremultiplied white(255, 255, 255, 255);
  const ui::Vector2f tex[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
  for (int i = 0; i < 4; ++i) {
    mesh.vertices.push_back(Vertex({centre_px.x + static_cast<float>(quad_dp[i].x) * px_per_dp,
                                    centre_px.y + static_cast<float>(quad_dp[i].y) * px_per_dp},
                                   white, tex[i]));
  }
  Triangle(mesh, 0, 1, 2);
  Triangle(mesh, 0, 2, 3);
  return mesh;
}

ui::Mesh AvatarCropDimMesh(const ui::Vector2f box_px, const ui::Vector2f centre_px, const float radius_px,
                           const int segments, const ui::ColourbPremultiplied shade) {
  ui::Mesh mesh;
  if (segments < 3 || radius_px <= 0.0f || box_px.x <= 0.0f || box_px.y <= 0.0f) {
    return mesh;
  }
  // The rectangle's corners with their angles about the centre, so a segment that spans one can include it.
  struct Corner {
    double angle;
    ui::Vector2f at;
  };
  std::vector<Corner> corners;
  for (const ui::Vector2f c :
       {ui::Vector2f(0, 0), ui::Vector2f(box_px.x, 0), ui::Vector2f(box_px.x, box_px.y), ui::Vector2f(0, box_px.y)}) {
    corners.push_back({NormalisedAngle(std::atan2(c.y - centre_px.y, c.x - centre_px.x)), c});
  }
  std::sort(corners.begin(), corners.end(), [](const Corner& a, const Corner& b) { return a.angle < b.angle; });

  auto add = [&](const ui::Vector2f p) {
    mesh.vertices.push_back(Vertex(p, shade));
    return static_cast<int>(mesh.vertices.size()) - 1;
  };
  for (int i = 0; i < segments; ++i) {
    const double a0 = kTwoPi * i / segments;
    const double a1 = kTwoPi * (i + 1) / segments;
    const int inner0 = add(OnCircle(centre_px, radius_px, a0));
    const int inner1 = add(OnCircle(centre_px, radius_px, a1));
    // Outer chain: the ray hit at a0, the corners strictly between, the ray hit at a1.
    std::vector<int> outer;
    outer.push_back(add(OnRectangle(box_px, centre_px, a0)));
    for (const Corner& c : corners) {
      if (c.angle > a0 + 1e-9 && c.angle < a1 - 1e-9) {
        outer.push_back(add(c.at));
      }
    }
    outer.push_back(add(OnRectangle(box_px, centre_px, a1)));
    for (size_t k = 0; k + 1 < outer.size(); ++k) {
      Triangle(mesh, inner0, outer[k], outer[k + 1]);
    }
    Triangle(mesh, inner0, outer.back(), inner1);
  }
  return mesh;
}

ui::Mesh AvatarCropRingMesh(const ui::Vector2f centre_px, const float radius_px, const float width_px,
                            const int segments, const ui::ColourbPremultiplied color) {
  ui::Mesh mesh;
  if (segments < 3 || radius_px <= 0.0f || width_px <= 0.0f) {
    return mesh;
  }
  for (int i = 0; i < segments; ++i) {
    const double a = kTwoPi * i / segments;
    mesh.vertices.push_back(Vertex(OnCircle(centre_px, radius_px, a), color));
    mesh.vertices.push_back(Vertex(OnCircle(centre_px, radius_px + width_px, a), color));
  }
  for (int i = 0; i < segments; ++i) {
    const int j = (i + 1) % segments;
    Triangle(mesh, 2 * i, 2 * i + 1, 2 * j + 1);
    Triangle(mesh, 2 * i, 2 * j + 1, 2 * j);
  }
  return mesh;
}

} // namespace pbr
