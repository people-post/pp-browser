#include "domain/media/CameraCandidates.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

using Ids = std::vector<uint32_t>;

// Device test 2026-09-30: a Mac listed its owner's iPhone (Continuity Camera) before the built-in
// camera, opened it for a call to that same phone, and the phone's side of the call broke.
TEST(CameraCandidatesTest, BorrowedCameraComesAfterTheDevicesOwn) {
  EXPECT_EQ(OrderCameraCandidates({{.id = 7, .borrowed = true}, {.id = 3}}), (Ids{3, 7}));
}

TEST(CameraCandidatesTest, FrontFacingFirstThenOsOrder) {
  EXPECT_EQ(OrderCameraCandidates({{.id = 1}, {.id = 2, .front_facing = true}, {.id = 3}}), (Ids{2, 1, 3}));
}

// A borrowed camera that says it is front-facing still does not beat an own back camera.
TEST(CameraCandidatesTest, OwnBackCameraBeatsABorrowedFrontCamera) {
  EXPECT_EQ(OrderCameraCandidates({{.id = 9, .front_facing = true, .borrowed = true}, {.id = 4}}), (Ids{4, 9}));
}

TEST(CameraCandidatesTest, BorrowedCameraIsStillUsedWhenItIsTheOnlyOne) {
  EXPECT_EQ(OrderCameraCandidates({{.id = 5, .borrowed = true}}), (Ids{5}));
  EXPECT_TRUE(OrderCameraCandidates({}).empty());
}

} // namespace
} // namespace pbr
