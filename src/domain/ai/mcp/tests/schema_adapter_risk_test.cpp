#include "domain/ai/mcp/SchemaAdapter.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SchemaAdapterRiskTest, UnannotatedToolDefaultsToDestructive) {
  // An MCP server that omits `annotations` entirely must not be trusted as read-only; the MCP
  // spec's own default for destructiveHint (when readOnlyHint isn't true) is true.
  McpTool tool;
  tool.name = "search_docs";
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "destructive");
}

TEST(SchemaAdapterRiskTest, ExplicitReadOnlyHintIsRead) {
  McpTool tool;
  tool.name = "search_docs";
  tool.annotations.read_only_hint = true;
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "read");
}

TEST(SchemaAdapterRiskTest, ReadOnlyHintFalseWithNoDestructiveHintIsDestructive) {
  McpTool tool;
  tool.name = "search_docs";
  tool.annotations.read_only_hint = false;
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "destructive");
}

TEST(SchemaAdapterRiskTest, ReadOnlyHintFalseWithDestructiveHintFalseIsWrite) {
  McpTool tool;
  tool.name = "search_docs";
  tool.annotations.read_only_hint = false;
  tool.annotations.destructive_hint = false;
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "write");
}

TEST(SchemaAdapterRiskTest, DestructiveHintWinsOverReadOnlyHint) {
  McpTool tool;
  tool.name = "tidy_up";
  tool.annotations.read_only_hint = true;
  tool.annotations.destructive_hint = true;
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "destructive");
}

TEST(SchemaAdapterRiskTest, DestructiveNameOverridesReadOnlyHint) {
  // A server cannot self-declare read-only-ness away from an obviously destructive name.
  McpTool tool;
  tool.name = "delete_file";
  tool.annotations.read_only_hint = true;
  EXPECT_EQ(SchemaAdapter::RiskClass(tool), "destructive");
}

} // namespace
} // namespace pbr
