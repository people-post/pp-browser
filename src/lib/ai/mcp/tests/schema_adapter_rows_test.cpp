#include "lib/ai/mcp/SchemaAdapter.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

// A tools/call result whose text block holds a JSON array becomes rows (what the mock MCP server
// used to check before it left release code, PB-081).
TEST(SchemaAdapterRowsTest, ATextBlockWithAJsonArrayBecomesRows) {
  Object text_block;
  text_block.set("type", "text");
  text_block.set("text", R"([{"name":"Ada","email":"ada@example.com"}])");
  Object result;
  result.set("content", ArrayValue({ObjectValue(std::move(text_block))}));

  auto rows = SchemaAdapter::ToolResultToRows(result);
  ASSERT_TRUE(static_cast<bool>(rows));
  const Array* array = asArray(*rows);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->elements.size(), 1u);
  const Object* row = asObject(array->elements[0]);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getString("name").value_or(""), "Ada");
}

} // namespace
} // namespace pbr
