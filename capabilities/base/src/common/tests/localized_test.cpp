#include "common/Localized.h"

#include <gtest/gtest.h>

#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Capabilities name keys; the app's catalogs answer through the translator it installs (capability-repos CR015).
TEST(LocalizedTest, AKeyReadsAsItselfWithNoTranslator) {
  SetTranslator({});
  EXPECT_EQ(Localized("call.status.reconnecting"), "call.status.reconnecting");
}

TEST(LocalizedTest, TheInstalledTranslatorAnswersWithItsArgs) {
  SetTranslator([](std::string_view key, const TextArgs& args) {
    return std::string(key) + ":" + (args.count("name") ? args.at("name") : "");
  });
  EXPECT_EQ(Localized("call.error.hop_unreachable_owner", {{"name", "Bob"}}), "call.error.hop_unreachable_owner:Bob");
  SetTranslator({});
}

} // namespace
} // namespace pbr
