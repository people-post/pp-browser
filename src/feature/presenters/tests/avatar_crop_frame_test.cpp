// The avatar crop page's arithmetic (PLAN-avatar-edit items 7, 10, 12): what the user frames in the circle is what is
// uploaded, at the angle they see, and no empty corner can show.
#include "feature/presenters/AvatarCropFrame.h"

#include "feature/presenters/TwoFingerGesture.h"
#include "foundation/platform/ImageSampleAffine.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

namespace pbr {
namespace {

using Point = AvatarCropFrame::Point;
constexpr double kPi = 3.14159265358979323846;

double Deg(const double d) {
  return d * kPi / 180.0;
}

// The source pixel coordinate (continuous, 0..w x 0..h) a screen point shows, found from the DRAWN quad only: the quad
// is the picture's corners TL, TR, BR, BL (texture (0,0), (1,0), (1,1), (0,1)), so q = TL + a (TR - TL) + b (BL - TL)
// and the pixel is (a w, b h). Independent of how AvatarCropFrame computes its mapping.
Point ImagePixelViaQuad(const AvatarCropFrame& frame, const Point q) {
  const auto quad = frame.Quad();
  const Point e1{quad[1].x - quad[0].x, quad[1].y - quad[0].y};
  const Point e2{quad[3].x - quad[0].x, quad[3].y - quad[0].y};
  const Point r{q.x - quad[0].x, q.y - quad[0].y};
  const double det = e1.x * e2.y - e1.y * e2.x;
  const double a = (r.x * e2.y - r.y * e2.x) / det;
  const double b = (e1.x * r.y - e1.y * r.x) / det;
  return {a * frame.image_w(), b * frame.image_h()};
}

Point ApplyAffine(const ImageAffine& m, const double u, const double v) {
  return {m.xu * u + m.xv * v + m.x0, m.yu * u + m.yv * v + m.y0};
}

// A random state reached through the public operations (so it is a state the page can really be in).
AvatarCropFrame RandomFrame(std::mt19937& rng) {
  std::uniform_int_distribution<int> side(1, 4000);
  std::uniform_int_distribution<int> thin(1, 30);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  int w = side(rng);
  int h = side(rng);
  if (unit(rng) < 0.15) {
    (unit(rng) < 0.5 ? w : h) = thin(rng);
  }
  AvatarCropFrame frame(w, h, 120.0 + 300.0 * unit(rng));
  std::uniform_real_distribution<double> pos(-200.0, 200.0);
  std::uniform_real_distribution<double> factor(0.5, 2.5);
  std::uniform_real_distribution<double> turn(-kPi, kPi);
  const int ops = 1 + static_cast<int>(unit(rng) * 8);
  for (int i = 0; i < ops; ++i) {
    switch (static_cast<int>(unit(rng) * 4)) {
      case 0:
        frame.Pan(pos(rng), pos(rng));
        break;
      case 1:
        frame.Pinch({pos(rng), pos(rng)}, factor(rng), turn(rng));
        break;
      case 2:
        frame.ZoomAt({pos(rng), pos(rng)}, factor(rng));
        break;
      default:
        frame.RotateQuarter();
        break;
    }
  }
  return frame;
}

TEST(AvatarCropFrameTest, StartsCentredWithTheShortSideSpanningTheCircle) {
  struct Case {
    int w, h;
  };
  for (const Case c : {Case{400, 400}, Case{300, 400}, Case{1600, 900}, Case{2000, 20}, Case{20, 2000}}) {
    const AvatarCropFrame frame(c.w, c.h, 300.0);
    EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
    EXPECT_DOUBLE_EQ(frame.theta(), 0.0);
    EXPECT_DOUBLE_EQ(frame.centre().x, 0.0);
    EXPECT_DOUBLE_EQ(frame.centre().y, 0.0);
    const auto q = frame.Quad();
    const double drawn_w = q[1].x - q[0].x;
    const double drawn_h = q[3].y - q[0].y;
    EXPECT_NEAR(std::min(drawn_w, drawn_h), 300.0, 1e-9) << c.w << "x" << c.h;
    EXPECT_NEAR(drawn_w / drawn_h, static_cast<double>(c.w) / c.h, 1e-9) << "aspect kept";
    EXPECT_NEAR(q[0].x + q[2].x, 0.0, 1e-9) << "centred";
    EXPECT_NEAR(q[0].y + q[2].y, 0.0, 1e-9);
  }
}

TEST(AvatarCropFrameTest, PanningIntoEachOfTheFourLimitsIsClamped) {
  // 1600 x 900 in a 300 dp circle at scale 2: r_img = 225, so |c.x| <= 575 and |c.y| <= 225.
  AvatarCropFrame frame(1600, 900, 300.0);
  frame.ZoomAt({0, 0}, 2.0);
  ASSERT_DOUBLE_EQ(frame.scale(), 2.0);
  frame.Pan(1e6, 0); // drag right: the picture moves right, the circle shows its left edge
  EXPECT_NEAR(frame.centre().x, -575.0, 1e-9);
  EXPECT_NEAR(ImagePixelViaQuad(frame, {-150.0, 0}).x, 0.0, 1e-6) << "the circle touches the left edge";
  frame.Pan(-1e6, 0);
  EXPECT_NEAR(frame.centre().x, 575.0, 1e-9);
  EXPECT_NEAR(ImagePixelViaQuad(frame, {150.0, 0}).x, 1600.0, 1e-6);
  frame.Pan(0, 1e6);
  EXPECT_NEAR(frame.centre().y, -225.0, 1e-9);
  EXPECT_NEAR(ImagePixelViaQuad(frame, {0, -150.0}).y, 0.0, 1e-6);
  frame.Pan(0, -1e6);
  EXPECT_NEAR(frame.centre().y, 225.0, 1e-9);
  EXPECT_NEAR(ImagePixelViaQuad(frame, {0, 150.0}).y, 900.0, 1e-6);
}

TEST(AvatarCropFrameTest, AtScaleOneTheShortSideCannotMoveAndAThinPictureStaysCovered) {
  AvatarCropFrame wide(1600, 900, 300.0);
  wide.Pan(0, 500);
  EXPECT_DOUBLE_EQ(wide.centre().y, 0.0);
  wide.Pan(-1e5, 0);
  EXPECT_NEAR(wide.centre().x, 350.0, 1e-9);
  AvatarCropFrame thin(3000, 7, 300.0);
  thin.Pan(37, -12);
  thin.Pinch({40, 40}, 0.3, Deg(33));
  for (int i = 0; i < 16; ++i) {
    const Point px = ImagePixelViaQuad(thin, {150.0 * std::cos(Deg(i * 22.5)), 150.0 * std::sin(Deg(i * 22.5))});
    EXPECT_GE(px.x, -1e-6);
    EXPECT_LE(px.x, 3000 + 1e-6);
    EXPECT_GE(px.y, -1e-6);
    EXPECT_LE(px.y, 7 + 1e-6);
  }
}

// The picture point under the fingers' midpoint stays under it (no clamp involved: a big picture zoomed in).
TEST(AvatarCropFrameTest, PinchKeepsThePicturePointUnderTheFocus) {
  struct Case {
    double factor, dtheta;
    const char* name;
  };
  for (const Case c : {Case{1.3, 0.0, "scale"}, Case{1.0, Deg(25), "turn"}, Case{0.8, Deg(-40), "both"},
                       Case{1.6, Deg(170), "both, large"}}) {
    AvatarCropFrame frame(2000, 2000, 300.0);
    frame.ZoomAt({0, 0}, 2.5);
    frame.Pan(10, -6);
    const Point focus{37.0, -21.0};
    const Point before = ImagePixelViaQuad(frame, focus);
    frame.Pinch(focus, c.factor, c.dtheta);
    const Point after = ImagePixelViaQuad(frame, focus);
    // Tolerance: 1e-6 image px (pure floating-point error; any formula slip is pixels off).
    EXPECT_NEAR(after.x, before.x, 1e-6) << c.name;
    EXPECT_NEAR(after.y, before.y, 1e-6) << c.name;
  }
}

TEST(AvatarCropFrameTest, ScaleStaysBetweenOneAndFive) {
  AvatarCropFrame frame(800, 600, 300.0);
  frame.ZoomAt({20, 20}, 100.0);
  EXPECT_DOUBLE_EQ(frame.scale(), 5.0);
  frame.Pinch({-20, 5}, 0.001, 0.2);
  EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
  frame.ZoomAt({0, 0}, 0.0); // ignored, not a zero scale
  frame.ZoomAt({0, 0}, -2.0);
  EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
}

TEST(AvatarCropFrameTest, FourQuarterTurnsRestoreTheState) {
  AvatarCropFrame frame(1200, 800, 280.0);
  frame.ZoomAt({30, -10}, 1.7);
  frame.Pinch({-12, 18}, 1.1, Deg(12));
  frame.Pan(-40, 25);
  const AvatarCropFrame start = frame;
  for (int i = 0; i < 4; ++i) {
    frame.RotateQuarter();
  }
  EXPECT_NEAR(frame.theta(), start.theta(), 1e-9);
  EXPECT_DOUBLE_EQ(frame.scale(), start.scale());
  EXPECT_DOUBLE_EQ(frame.centre().x, start.centre().x);
  EXPECT_DOUBLE_EQ(frame.centre().y, start.centre().y);
  for (int i = 0; i < 4; ++i) {
    EXPECT_NEAR(frame.Quad()[i].x, start.Quad()[i].x, 1e-6);
    EXPECT_NEAR(frame.Quad()[i].y, start.Quad()[i].y, 1e-6);
  }
}

TEST(AvatarCropFrameTest, AQuarterTurnIsClockwiseAndKeepsThePointUnderTheCentre) {
  AvatarCropFrame frame(1200, 800, 280.0);
  frame.ZoomAt({0, 0}, 2.0);
  frame.Pan(50, 30);
  const Point centre_before = ImagePixelViaQuad(frame, {0, 0});
  frame.RotateQuarter();
  const Point centre_after = ImagePixelViaQuad(frame, {0, 0});
  EXPECT_NEAR(centre_after.x, centre_before.x, 1e-6);
  EXPECT_NEAR(centre_after.y, centre_before.y, 1e-6);
  const auto q = frame.Quad();
  // The picture's top edge (TL -> TR) pointed right; after a clockwise quarter it points down (y-down screen).
  EXPECT_NEAR(q[1].x - q[0].x, 0.0, 1e-6);
  EXPECT_GT(q[1].y - q[0].y, 0.0);
}

TEST(AvatarCropFrameTest, InAnyStateTheWholeCircleIsInsideThePicture) {
  std::mt19937 rng(20261110);
  for (int n = 0; n < 300; ++n) {
    const AvatarCropFrame frame = RandomFrame(rng);
    ASSERT_GE(frame.scale(), 1.0);
    ASSERT_LE(frame.scale(), 5.0);
    const double r = frame.viewport_diameter() / 2.0;
    for (int i = 0; i < 64; ++i) {
      const double a = 2.0 * kPi * i / 64.0;
      const Point px = ImagePixelViaQuad(frame, {r * std::cos(a), r * std::sin(a)});
      // 1e-6 px of floating-point slack on the limit itself.
      ASSERT_GE(px.x, -1e-6) << "state " << n << " " << frame.image_w() << "x" << frame.image_h();
      ASSERT_LE(px.x, frame.image_w() + 1e-6) << "state " << n;
      ASSERT_GE(px.y, -1e-6) << "state " << n;
      ASSERT_LE(px.y, frame.image_h() + 1e-6) << "state " << n;
    }
  }
}

// THE CONSISTENCY TEST: what the page draws in the circle and what the upload samples are the same part of the picture.
// The circle's centre and 8 points on it, through the drawn quad (inverted) and through the output affine.
TEST(AvatarCropFrameTest, TheDrawnQuadAndTheOutputAffineShowTheSamePixels) {
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> out_side(16, 1024);
  for (int n = 0; n < 250; ++n) {
    const AvatarCropFrame frame = RandomFrame(rng);
    const int side = out_side(rng);
    const ImageAffine affine = frame.OutputAffine(side);
    const double r_screen = frame.viewport_diameter() / 2.0;
    const double half = side / 2.0;
    for (int i = -1; i < 8; ++i) {
      const double a = i < 0 ? 0.0 : 2.0 * kPi * i / 8.0;
      const double t = i < 0 ? 0.0 : 1.0; // -1: the centre
      const Point drawn = ImagePixelViaQuad(frame, {t * r_screen * std::cos(a), t * r_screen * std::sin(a)});
      const Point sampled = ApplyAffine(affine, half + t * half * std::cos(a), half + t * half * std::sin(a));
      ASSERT_NEAR(sampled.x, drawn.x, 0.5) << "state " << n << " point " << i;
      ASSERT_NEAR(sampled.y, drawn.y, 0.5) << "state " << n << " point " << i;
    }
  }
}

TEST(AvatarCropFrameTest, TheStartingOutputIsTheCentredLargestSquare) {
  const AvatarCropFrame wide(300, 200, 250.0);
  const ImageAffine a = wide.OutputAffine(200);
  EXPECT_NEAR(a.xu, 1.0, 1e-12);
  EXPECT_NEAR(a.xv, 0.0, 1e-12);
  EXPECT_NEAR(a.x0, 50.0, 1e-9);
  EXPECT_NEAR(a.yu, 0.0, 1e-12);
  EXPECT_NEAR(a.yv, 1.0, 1e-12);
  EXPECT_NEAR(a.y0, 0.0, 1e-9);
  const ImageAffine tall = AvatarCropFrame(200, 300, 250.0).OutputAffine(100); // 200 px square into 100: 2 per px
  EXPECT_NEAR(tall.xu, 2.0, 1e-12);
  EXPECT_NEAR(tall.x0, 0.0, 1e-9);
  EXPECT_NEAR(tall.yv, 2.0, 1e-12);
  EXPECT_NEAR(tall.y0, 50.0, 1e-9);
  EXPECT_DOUBLE_EQ(wide.CircleDiameterImagePx(), 200.0);
}

TEST(AvatarCropFrameTest, DegenerateSizesAndArgumentsNeverGiveNaN) {
  AvatarCropFrame tiny(1, 1, 0.0);
  AvatarCropFrame empty(0, -5, std::nan(""));
  for (AvatarCropFrame* f : {&tiny, &empty}) {
    f->Pan(10, 10);
    f->Pinch({3, 3}, 2.0, 1.0);
    f->Pan(std::nan(""), 1.0);
    f->Pinch({std::nan(""), 0}, 2.0, 0.0);
    f->Pinch({0, 0}, std::numeric_limits<double>::infinity(), 0.0);
    f->RotateQuarter();
    EXPECT_EQ(f->image_w(), 1);
    EXPECT_EQ(f->image_h(), 1);
    EXPECT_DOUBLE_EQ(f->viewport_diameter(), 1.0);
    for (const Point p : f->Quad()) {
      EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
    }
    const ImageAffine a = f->OutputAffine(0);
    for (const double v : {a.xu, a.xv, a.x0, a.yu, a.yv, a.y0}) {
      EXPECT_TRUE(std::isfinite(v));
    }
    EXPECT_TRUE(std::isfinite(f->theta()));
    EXPECT_TRUE(std::isfinite(f->centre().x) && std::isfinite(f->centre().y));
  }
}

// SDL reports the two fingers one at a time, so a pure twist is a tiny shrink then a tiny growth per frame. At the
// scale limit the shrink is clamped away and the growth kept: a second-order creep (1 - cos(step / 2) per step). With
// real event steps (about a degree) it stays far below anything visible; this pins that bound.
TEST(AvatarCropFrameTest, ATwistAtTheSmallestScaleBarelyZoomsIn) {
  AvatarCropFrame frame(400, 400, 200.0);
  TwoFingerGesture gesture;
  const auto apply = [&](const FingerGestureDelta& d) {
    frame.Pan(d.pan_dx, d.pan_dy);
    if (d.HasPinch()) {
      frame.Pinch({d.focus_x, d.focus_y}, d.scale, d.rotation);
    }
  };
  apply(gesture.OnEvent(1, -60, 0, FingerEventKind::Down));
  apply(gesture.OnEvent(2, 60, 0, FingerEventKind::Down));
  for (int step = 1; step <= 90; ++step) {
    const double t = Deg(step);
    apply(gesture.OnEvent(2, 60 * std::cos(t), 60 * std::sin(t), FingerEventKind::Move));
    apply(gesture.OnEvent(1, -60 * std::cos(t), -60 * std::sin(t), FingerEventKind::Move));
  }
  EXPECT_NEAR(frame.theta(), Deg(90), 1e-9);
  EXPECT_LT(frame.scale(), 1.004) << "a quarter twist in 1 degree steps: under 0.4% zoom";
}

// ---- Through the sampler: the uploaded picture -----------------------------------------------------------------

struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kYellow{255, 255, 0};

// Top-left red, top-right green, bottom-left blue, bottom-right yellow.
SDL_Surface* Quadrants(const int w, const int h) {
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
  const auto fill = [&](const int x, const int y, const int fw, const int fh, const Rgb c) {
    const SDL_Rect r{x, y, fw, fh};
    SDL_FillSurfaceRect(s, &r, SDL_MapSurfaceRGB(s, c.r, c.g, c.b));
  };
  fill(0, 0, w / 2, h / 2, kRed);
  fill(w / 2, 0, w - w / 2, h / 2, kGreen);
  fill(0, h / 2, w / 2, h - h / 2, kBlue);
  fill(w / 2, h / 2, w - w / 2, h - h / 2, kYellow);
  return s;
}

Rgb PixelAt(SDL_Surface* s, const int x, const int y) {
  Rgb c{};
  uint8_t a = 0;
  EXPECT_TRUE(SDL_ReadSurfacePixel(s, x, y, &c.r, &c.g, &c.b, &a));
  return c;
}

bool Same(const Rgb a, const Rgb b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

// Which quadrant colour a source pixel coordinate is in (for the drawn side of the direction test).
Rgb QuadrantOf(const Point px, const int w, const int h) {
  const bool left = px.x < w / 2.0;
  const bool top = px.y < h / 2.0;
  return top ? (left ? kRed : kGreen) : (left ? kBlue : kYellow);
}

// THE DIRECTION TEST: two fingers whose line turns 30 degrees clockwise on a y-down screen turn the drawn picture 30
// degrees clockwise, and the uploaded picture shows the same turn.
TEST(AvatarCropFrameTest, TwoFingersTurningClockwiseTurnTheDrawnAndTheUploadedPictureClockwise) {
  constexpr int kW = 400;
  AvatarCropFrame frame(kW, kW, 200.0);
  frame.ZoomAt({0, 0}, 2.0); // off the scale limit, so the per-event ratios multiply back to exactly 1 (see below)
  TwoFingerGesture gesture;
  const auto apply = [&](const FingerGestureDelta& d) {
    frame.Pan(d.pan_dx, d.pan_dy);
    if (d.HasPinch()) {
      frame.Pinch({d.focus_x, d.focus_y}, d.scale, d.rotation);
    }
  };
  apply(gesture.OnEvent(1, -60, 0, FingerEventKind::Down));
  apply(gesture.OnEvent(2, 60, 0, FingerEventKind::Down));
  for (int step = 1; step <= 3; ++step) { // 10 degrees a step, one finger at a time as SDL reports them
    const double t = Deg(10.0 * step);
    apply(gesture.OnEvent(2, 60 * std::cos(t), 60 * std::sin(t), FingerEventKind::Move));
    apply(gesture.OnEvent(1, -60 * std::cos(t), -60 * std::sin(t), FingerEventKind::Move));
  }
  ASSERT_NEAR(frame.theta(), Deg(30), 1e-9);
  ASSERT_NEAR(frame.scale(), 2.0, 1e-9);
  ASSERT_NEAR(frame.centre().x, 0.0, 1e-9) << "turned about the fingers' midpoint, the circle's centre";
  ASSERT_NEAR(frame.centre().y, 0.0, 1e-9);

  // Drawn: the picture's top edge now points 30 degrees below "right", i.e. clockwise.
  const auto q = frame.Quad();
  EXPECT_NEAR(std::atan2(q[1].y - q[0].y, q[1].x - q[0].x), Deg(30), 1e-9);
  // Quadrant centres were at -135 (red), -45 (green), 45 (yellow), 135 (blue) degrees; turned clockwise by 30 they
  // are at -105, -15, 75, 165. At -80 degrees: red (turned the wrong way it would be green); at -170: blue (wrong way:
  // red).
  const auto at = [](const double deg, const double radius) {
    return Point{radius * std::cos(Deg(deg)), radius * std::sin(Deg(deg))};
  };
  EXPECT_TRUE(Same(QuadrantOf(ImagePixelViaQuad(frame, at(-80, 50)), kW, kW), kRed));
  EXPECT_TRUE(Same(QuadrantOf(ImagePixelViaQuad(frame, at(-170, 50)), kW, kW), kBlue));
  EXPECT_TRUE(Same(QuadrantOf(ImagePixelViaQuad(frame, at(10, 50)), kW, kW), kGreen));
  EXPECT_TRUE(Same(QuadrantOf(ImagePixelViaQuad(frame, at(100, 50)), kW, kW), kYellow));

  // Uploaded: the same places in the 200 x 200 output.
  SDL_Surface* src = Quadrants(kW, kW);
  SDL_Surface* out = SampleAffine(src, 200, frame.OutputAffine(200));
  ASSERT_NE(out, nullptr);
  const auto out_at = [&](const double deg) {
    const Point p = at(deg, 50);
    return PixelAt(out, static_cast<int>(100 + p.x), static_cast<int>(100 + p.y));
  };
  EXPECT_TRUE(Same(out_at(-80), kRed));
  EXPECT_TRUE(Same(out_at(-170), kBlue));
  EXPECT_TRUE(Same(out_at(10), kGreen));
  EXPECT_TRUE(Same(out_at(100), kYellow));
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

// A quarter turn through the frame and the sampler = the bitmap turned 90 degrees clockwise, then centre-cropped.
TEST(AvatarCropFrameTest, AQuarterTurnUploadsTheBitmapTurnedAndCentreCropped) {
  constexpr int kW = 300;
  constexpr int kH = 200;
  SDL_Surface* src = SDL_CreateSurface(kW, kH, SDL_PIXELFORMAT_RGB24);
  for (int y = 0; y < kH; ++y) {
    auto* row = static_cast<uint8_t*>(src->pixels) + static_cast<size_t>(y) * src->pitch;
    for (int x = 0; x < kW; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(x & 0xFF);
      row[x * 3 + 1] = static_cast<uint8_t>(y);
      row[x * 3 + 2] = static_cast<uint8_t>((x >> 8) * 200 + (x * 7 + y * 13) % 50);
    }
  }
  AvatarCropFrame frame(kW, kH, 240.0);
  frame.RotateQuarter();
  SDL_Surface* out = SampleAffine(src, kH, frame.OutputAffine(kH));
  ASSERT_NE(out, nullptr);
  // Turned clockwise: kH wide, kW tall, turned(dx, dy) = src(dy, kH - 1 - dx); the centred kH square is rows 50..249.
  const int crop_top = (kW - kH) / 2;
  int worst = 0;
  for (int v = 0; v < kH; ++v) {
    for (int u = 0; u < kH; ++u) {
      const Rgb want = PixelAt(src, v + crop_top, kH - 1 - u);
      const Rgb got = PixelAt(out, u, v);
      worst = std::max({worst, std::abs(want.r - got.r), std::abs(want.g - got.g), std::abs(want.b - got.b)});
    }
  }
  EXPECT_LE(worst, 1) << "within rounding";
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

// Zoomed in and dragged to the top-left corner: the upload is the red quadrant only.
TEST(AvatarCropFrameTest, DraggedToACornerTheUploadIsThatQuadrant) {
  SDL_Surface* src = Quadrants(400, 300);
  AvatarCropFrame frame(400, 300, 200.0);
  frame.ZoomAt({0, 0}, 2.5);
  frame.Pan(1e4, 1e4); // the picture moves down-right: the circle shows its top-left
  SDL_Surface* out = SampleAffine(src, 64, frame.OutputAffine(64));
  ASSERT_NE(out, nullptr);
  for (int v = 0; v < 64; v += 7) {
    for (int u = 0; u < 64; u += 7) {
      EXPECT_TRUE(Same(PixelAt(out, u, v), kRed)) << u << "," << v;
    }
  }
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

} // namespace
} // namespace pbr
