#include "domain/ai/IToolProvider.h"
#include "domain/ai/ToolRegistry.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

pbr::Object EmptyObjectSchema() {
  pbr::Object schema;
  schema.set("type", "object");
  schema.set("properties", pbr::Object{});
  return schema;
}

class FakeToolProvider : public pbr::IToolProvider {
public:
  std::string Id() const override { return "fake"; }

  std::vector<pbr::ToolDescriptor> ListTools() override {
    pbr::ToolDescriptor tool;
    tool.definition.name = "echo_tool";
    tool.definition.description = "Echoes arguments";
    tool.definition.parameters = EmptyObjectSchema();
    tool.meta.domain = "general";
    tool.meta.risk = "read";
    tool.execute = [](const pbr::Object& arguments) -> pbr::Roe<std::string> {
      return pbr::DumpJson(arguments);
    };
    return {std::move(tool)};
  }
};

} // namespace

TEST(ToolRegistryProviderTest, RegisterProviderFillsProviderMetaAndSummary) {
  pbr::ToolRegistry registry;
  FakeToolProvider provider;
  registry.RegisterProvider(provider);

  ASSERT_EQ(registry.Tools().size(), 1u);
  EXPECT_EQ(registry.Tools()[0].meta.provider, "fake");
  EXPECT_EQ(registry.Tools()[0].meta.domain, "general");

  const std::string summary = registry.SummaryForPrompt();
  EXPECT_NE(summary.find("echo_tool [general, read]"), std::string::npos);
  EXPECT_NE(summary.find("Echoes arguments"), std::string::npos);

  pbr::Object args;
  args.set("ok", true);
  auto result = registry.Execute("echo_tool", args);
  ASSERT_TRUE(result);
  EXPECT_NE(result->find("\"ok\":true"), std::string::npos);
}

TEST(ToolRegistryProviderTest, RegisterProviderSkipsDuplicateNames) {
  pbr::ToolRegistry registry;
  pbr::ToolDescriptor first;
  first.definition.name = "echo_tool";
  first.definition.description = "first";
  first.definition.parameters = pbr::Object{};
  first.execute = [](const pbr::Object&) -> pbr::Roe<std::string> { return std::string("first"); };
  registry.Register(std::move(first));

  FakeToolProvider provider;
  registry.RegisterProvider(provider);
  ASSERT_EQ(registry.Tools().size(), 1u);
  EXPECT_EQ(registry.Tools()[0].definition.description, "first");
}
