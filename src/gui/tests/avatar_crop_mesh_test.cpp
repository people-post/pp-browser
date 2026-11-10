// The crop element's geometry (PLAN-avatar-edit item 9): the picture as one quad at the frame's corners, the dim that
// covers exactly the area outside the circle, the circle line.
#include "gui/avatar/AvatarCropMesh.h"

#include <gtest/gtest.h>

#include <cmath>

namespace pbr {
namespace {

double TriangleArea(const ui::Mesh& mesh, const size_t first) {
  const auto& a = mesh.vertices[mesh.indices[first]].position;
  const auto& b = mesh.vertices[mesh.indices[first + 1]].position;
  const auto& c = mesh.vertices[mesh.indices[first + 2]].position;
  return 0.5 * std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

double TotalArea(const ui::Mesh& mesh) {
  double sum = 0.0;
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    sum += TriangleArea(mesh, i);
  }
  return sum;
}

TEST(AvatarCropMeshTest, TheDimIsTheRectangleMinusTheCircle) {
  const ui::Vector2f box(600, 900);
  const ui::Vector2f centre(300, 450);
  const float radius = 250.0f;
  const ui::Mesh dim = AvatarCropDimMesh(box, centre, radius, 64, ui::ColourbPremultiplied(0, 0, 0, 150));
  const double polygon = 0.5 * 64 * radius * radius * std::sin(2.0 * 3.14159265358979323846 / 64);
  EXPECT_NEAR(TotalArea(dim), 600.0 * 900.0 - polygon, 1.0);
  for (const ui::Vertex& v : dim.vertices) {
    EXPECT_GE(v.position.x, -0.01f);
    EXPECT_LE(v.position.x, 600.01f);
    EXPECT_GE(v.position.y, -0.01f);
    EXPECT_LE(v.position.y, 900.01f);
  }
}

TEST(AvatarCropMeshTest, TheDimWorksForACircleOffCentreAndAWideBox) {
  const ui::Vector2f box(1000, 300);
  const ui::Vector2f centre(400, 120);
  const ui::Mesh dim = AvatarCropDimMesh(box, centre, 100.0f, 64, ui::ColourbPremultiplied(0, 0, 0, 150));
  const double polygon = 0.5 * 64 * 100.0 * 100.0 * std::sin(2.0 * 3.14159265358979323846 / 64);
  EXPECT_NEAR(TotalArea(dim), 1000.0 * 300.0 - polygon, 1.0);
}

TEST(AvatarCropMeshTest, NothingIsDrawnForADegenerateCircleOrBox) {
  EXPECT_TRUE(AvatarCropDimMesh({100, 100}, {50, 50}, 0.0f, 64, {}).indices.empty());
  EXPECT_TRUE(AvatarCropDimMesh({0, 100}, {50, 50}, 10.0f, 64, {}).indices.empty());
  EXPECT_TRUE(AvatarCropRingMesh({50, 50}, 10.0f, 0.0f, 64, {}).indices.empty());
}

TEST(AvatarCropMeshTest, TheRingIsAnAnnulusOfTheGivenWidth) {
  const ui::Mesh ring = AvatarCropRingMesh({100, 100}, 80.0f, 2.0f, 64, ui::ColourbPremultiplied(255, 255, 255, 255));
  const double area = 0.5 * 64 * (82.0 * 82.0 - 80.0 * 80.0) * std::sin(2.0 * 3.14159265358979323846 / 64);
  EXPECT_NEAR(TotalArea(ring), area, 0.5);
}

TEST(AvatarCropMeshTest, TheImageIsOneQuadAtTheFramesCornersWithTheTextureCornersInOrder) {
  AvatarCropFrame frame(400, 300, 200);
  frame.Pinch({0, 0}, 1.0, 0.5235987755982988); // 30 degrees, as the spike's hard-coded state
  const auto quad = frame.Quad();
  const ui::Mesh image = AvatarCropImageMesh(quad, 2.0f, {500, 400});
  ASSERT_EQ(image.vertices.size(), 4u);
  ASSERT_EQ(image.indices.size(), 6u);
  const ui::Vector2f tex[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int i = 0; i < 4; ++i) {
    EXPECT_NEAR(image.vertices[i].position.x, 500 + 2.0 * quad[i].x, 1e-3);
    EXPECT_NEAR(image.vertices[i].position.y, 400 + 2.0 * quad[i].y, 1e-3);
    EXPECT_EQ(image.vertices[i].tex_coord, tex[i]);
  }
  // The quad is the picture's rectangle, turned: its area is k^2 * w * h, k = D / min(w, h) = 200 / 300 in dp, 2 px/dp.
  const double k = 2.0 * 200.0 / 300.0;
  EXPECT_NEAR(TotalArea(image), k * k * 400.0 * 300.0, 1.0);
}

} // namespace
} // namespace pbr
