#include "common/ui/RmlEscape.h"

#include <gtest/gtest.h>

using namespace pbr;

TEST(RmlEscapeTest, MarkupCharactersBecomeEntities) {
  EXPECT_EQ(EscapeRml("a < b & c > \"d\""), "a &lt; b &amp; c &gt; &quot;d&quot;");
  EXPECT_EQ(EscapeRml("<b>bold</b>"), "&lt;b&gt;bold&lt;/b&gt;");
}

TEST(RmlEscapeTest, BracesCannotBecomeADataExpression) {
  // Inner RML set from a string goes through the data-binding pass: literal braces must not form `{{…}}`.
  EXPECT_EQ(EscapeRml("{{profile_nickname}}"), "&#123;&#123;profile_nickname&#125;&#125;");
}

TEST(RmlEscapeTest, PlainTextIsUnchanged) {
  EXPECT_EQ(EscapeRml("王小明 — 2026-10-07 ✓"), "王小明 — 2026-10-07 ✓");
  EXPECT_EQ(EscapeRml(""), "");
}
