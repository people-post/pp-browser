#include "base/mesh/channel/MessageReassembly.h"

#include <gtest/gtest.h>

namespace pbr::amp {
namespace {

ChannelFragBody MakeFrag(uint64_t msg_id, uint16_t index, uint16_t count, std::vector<uint8_t> chunk,
                         uint32_t total_len) {
  ChannelFragBody frag;
  frag.msg_id = msg_id;
  frag.frag_index = index;
  frag.frag_count = count;
  frag.total_len = total_len;
  frag.chunk = std::move(chunk);
  return frag;
}

TEST(MessageReassemblyTest, AssemblesInOrder) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(1, 0, 2, {'a', 'b'}, 4);
  auto f1 = MakeFrag(1, 1, 2, {'c', 'd'}, 4);

  auto p0 = asmbl.Push(f0, 0);
  ASSERT_TRUE(static_cast<bool>(p0));
  EXPECT_FALSE(p0->has_value());

  auto p1 = asmbl.Push(f1, 0);
  ASSERT_TRUE(static_cast<bool>(p1));
  ASSERT_TRUE(p1->has_value());
  const auto assembled = p1->value();
  EXPECT_EQ(assembled, (std::vector<uint8_t>{'a', 'b', 'c', 'd'}));
}

TEST(MessageReassemblyTest, DuplicateFragDropped) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(2, 0, 2, {'x'}, 2);
  auto f0dup = MakeFrag(2, 0, 2, {'x'}, 2);
  auto f1 = MakeFrag(2, 1, 2, {'y'}, 2);

  ASSERT_TRUE(static_cast<bool>(asmbl.Push(f0, 0)));
  auto dup = asmbl.Push(f0dup, 0);
  ASSERT_TRUE(static_cast<bool>(dup));
  EXPECT_FALSE(dup->has_value());
  auto done = asmbl.Push(f1, 0);
  ASSERT_TRUE(static_cast<bool>(done));
  ASSERT_TRUE(done->has_value());
  const auto assembled = done->value();
  EXPECT_EQ(assembled, (std::vector<uint8_t>{'x', 'y'}));
}

} // namespace
} // namespace pbr::amp
