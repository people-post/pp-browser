#include "common/BoundedRead.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace pbr {
namespace {

// PB-063: a file at the cap is read whole; one byte more is refused, not loaded.
TEST(BoundedReadTest, AFileAtTheCapIsReadAndOneByteMoreIsRefused) {
  const std::string at_cap(200 * 1024, 'x');
  std::istringstream fits(at_cap);
  auto read = ReadBounded(fits, at_cap.size());
  ASSERT_TRUE(static_cast<bool>(read)) << read.error().message;
  EXPECT_EQ(*read, at_cap);

  std::istringstream over(at_cap + "y");
  EXPECT_FALSE(static_cast<bool>(ReadBounded(over, at_cap.size())));
  std::istringstream over_bytes(at_cap + "y");
  EXPECT_FALSE(static_cast<bool>(ReadBoundedBytes(over_bytes, at_cap.size())));
}

TEST(BoundedReadTest, AnEmptyStreamIsEmpty) {
  std::istringstream empty;
  auto read = ReadBoundedBytes(empty, 16);
  ASSERT_TRUE(static_cast<bool>(read));
  EXPECT_TRUE(read->empty());
}

} // namespace
} // namespace pbr
