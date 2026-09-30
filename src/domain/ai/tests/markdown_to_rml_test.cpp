#include "domain/ai/MarkdownToRml.h"

#include "domain/ai/RmlValidator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using pbr::MarkdownToRml;

std::string Rml(const std::string& md) { return MarkdownToRml(md).rml; }

std::string LinkSpan(int index, const std::string& label) {
  return "<span class=\"chat-link\" data-event-click=\"open_chat_link('__ENTRY__', " + std::to_string(index) + ")\">" +
         label + "</span>";
}

// Returns an empty string when every tag is closed and properly nested, else a description.
// Any '<' in the output must start a tag: text '<' is always escaped by the converter.
std::string CheckBalanced(const std::string& rml) {
  std::vector<std::string> stack;
  size_t i = 0;
  while (i < rml.size()) {
    if (rml[i] != '<') {
      ++i;
      continue;
    }
    const size_t end = rml.find('>', i);
    if (end == std::string::npos) {
      return "unterminated tag at " + std::to_string(i);
    }
    std::string body = rml.substr(i + 1, end - i - 1);
    i = end + 1;
    if (!body.empty() && body.back() == '/') {
      continue; // <br />
    }
    if (!body.empty() && body[0] == '/') {
      const std::string name = body.substr(1);
      if (stack.empty() || stack.back() != name) {
        return "mismatched </" + name + ">";
      }
      stack.pop_back();
    } else {
      stack.push_back(body.substr(0, body.find_first_of(" \t")));
    }
  }
  return stack.empty() ? "" : "unclosed <" + stack.back() + ">";
}

void ExpectSafe(const std::string& rml) {
  EXPECT_TRUE(pbr::RmlValidator::ValidateFragment(rml).ok) << rml;
  EXPECT_EQ(CheckBalanced(rml), "") << rml;
  EXPECT_EQ(rml.find("{{"), std::string::npos) << rml;
}

} // namespace

TEST(MarkdownToRmlTest, ParagraphsAndLineBreaks) {
  EXPECT_EQ(Rml("Hello\nworld\n\nSecond"), "<p>Hello<br />world</p><p>Second</p>");
  EXPECT_EQ(Rml(""), "");
  EXPECT_EQ(Rml("a\r\nb"), "<p>a<br />b</p>");
}

TEST(MarkdownToRmlTest, Headings) {
  EXPECT_EQ(Rml("# A\n## B\n### C\n###### D"), "<h1>A</h1><h2>B</h2><h3>C</h3><h3>D</h3>");
  EXPECT_EQ(Rml("####### seven"), "<p>####### seven</p>");
  EXPECT_EQ(Rml("#nospace"), "<p>#nospace</p>");
  EXPECT_EQ(Rml("# **bold** title"), "<h1><strong>bold</strong> title</h1>");
}

TEST(MarkdownToRmlTest, Lists) {
  EXPECT_EQ(Rml("- a\n* b\n+ c"), "<ul><li>a</li><li>b</li><li>c</li></ul>");
  EXPECT_EQ(Rml("1. a\n2. b"), "<ol><li>a</li><li>b</li></ol>");
  EXPECT_EQ(Rml("- a\n  - b\n    - c\n- d"), "<ul><li>a<ul><li>b<ul><li>c</li></ul></li></ul></li><li>d</li></ul>");
  EXPECT_EQ(Rml("1. a\n   - b\n2. c"), "<ol><li>a<ul><li>b</li></ul></li><li>c</li></ol>");
  EXPECT_EQ(Rml("- a\n\n- b"), "<ul><li>a</li><li>b</li></ul>");
  EXPECT_EQ(Rml("- a\n  more\n- b"), "<ul><li>a<br />more</li><li>b</li></ul>");
  EXPECT_EQ(Rml("Intro:\n- a"), "<p>Intro:</p><ul><li>a</li></ul>");
  EXPECT_EQ(Rml("- a\n1. b"), "<ul><li>a</li></ul><ol><li>b</li></ol>");
  EXPECT_EQ(Rml("- a\n\ntext"), "<ul><li>a</li></ul><p>text</p>");
  EXPECT_EQ(Rml("**not a list**"), "<p><strong>not a list</strong></p>");
}

TEST(MarkdownToRmlTest, ListDepthIsCapped) {
  std::string md;
  for (int depth = 0; depth < 10; ++depth) {
    md += std::string(static_cast<size_t>(depth) * 2, ' ') + "- x\n";
  }
  const std::string rml = Rml(md);
  size_t max_depth = 0;
  size_t depth = 0;
  for (size_t i = 0; i + 3 < rml.size(); ++i) {
    if (rml.compare(i, 4, "<ul>") == 0) {
      max_depth = std::max(max_depth, ++depth);
    } else if (rml.compare(i, 5, "</ul>") == 0) {
      --depth;
    }
  }
  EXPECT_EQ(max_depth, 6u);
  ExpectSafe(rml);
}

TEST(MarkdownToRmlTest, FencedCode) {
  EXPECT_EQ(Rml("```cpp\nint x = 1 < 2;\n\n  y();\n```"), "<div class=\"code-block\">int x = 1 &lt; 2;\n\n  y();</div>");
  EXPECT_EQ(Rml("~~~\n**x**\n~~~\nafter"), "<div class=\"code-block\">**x**</div><p>after</p>");
  EXPECT_EQ(Rml("```\nunterminated\n"), "<div class=\"code-block\">unterminated</div>");
  EXPECT_EQ(Rml("```"), "<div class=\"code-block\"></div>");
  EXPECT_EQ(Rml("```\n{{a}}\n```"), "<div class=\"code-block\">&#123;&#123;a&#125;&#125;</div>");
}

TEST(MarkdownToRmlTest, Blockquote) {
  EXPECT_EQ(Rml("> a **b**\n> c"), "<blockquote class=\"chat-quote\"><p>a <strong>b</strong><br />c</p></blockquote>");
  EXPECT_EQ(Rml("> a\n\nb"), "<blockquote class=\"chat-quote\"><p>a</p></blockquote><p>b</p>");
  EXPECT_EQ(Rml("> > nested"), "<blockquote class=\"chat-quote\"><p>&gt; nested</p></blockquote>");
}

TEST(MarkdownToRmlTest, Tables) {
  EXPECT_EQ(Rml("| A | B |\n|---|:-:|\n| 1 | **2** |\n| 3 |"),
            "<table class=\"chat-table\"><thead><tr><th>A</th><th>B</th></tr></thead><tbody>"
            "<tr><td>1</td><td><strong>2</strong></td></tr><tr><td>3</td><td></td></tr></tbody></table>");
  EXPECT_EQ(Rml("A | B\n--|--\na \\| b | c\n\nafter"),
            "<table class=\"chat-table\"><thead><tr><th>A</th><th>B</th></tr></thead><tbody>"
            "<tr><td>a | b</td><td>c</td></tr></tbody></table><p>after</p>");
  // No delimiter row: ordinary paragraph.
  EXPECT_EQ(Rml("| A | B |\n| 1 | 2 |"), "<p>| A | B |<br />| 1 | 2 |</p>");
  // Delimiter column count must match the header.
  EXPECT_EQ(Rml("| A | B |\n|---|"), "<p>| A | B |<br />|---|</p>");
}

TEST(MarkdownToRmlTest, ThematicBreakEmitsNothing) {
  EXPECT_EQ(Rml("a\n\n---\n\nb\n\n***\n\n___"), "<p>a</p><p>b</p>");
}

TEST(MarkdownToRmlTest, InlineFormatting) {
  EXPECT_EQ(Rml("**b** __b__ *e* _e_ `c`"),
            "<p><strong>b</strong> <strong>b</strong> <em>e</em> <em>e</em> <span class=\"inline-code\">c</span></p>");
  EXPECT_EQ(Rml("***both***"), "<p><em><strong>both</strong></em></p>");
  EXPECT_EQ(Rml("**a *b* c**"), "<p><strong>a <em>b</em> c</strong></p>");
  EXPECT_EQ(Rml("snake_case_name and 2 * 3 * 4"), "<p>snake_case_name and 2 * 3 * 4</p>");
  EXPECT_EQ(Rml("`a *b* <c>`"), "<p><span class=\"inline-code\">a *b* &lt;c&gt;</span></p>");
  EXPECT_EQ(Rml("``a`b``"), "<p><span class=\"inline-code\">a`b</span></p>");
  EXPECT_EQ(Rml("\\*x\\* \\_y\\_ \\q"), "<p>*x* _y_ \\q</p>");
  // Crossing markers: the inner unmatched one stays literal.
  EXPECT_EQ(Rml("**a *b** c*"), "<p><strong>a *b</strong> c*</p>");
  // Unsupported syntax is literal.
  EXPECT_EQ(Rml("~~gone~~ [ ] task"), "<p>~~gone~~ [ ] task</p>");
}

TEST(MarkdownToRmlTest, HttpsLinks) {
  auto r = MarkdownToRml("[a](https://x.com/p) and [b **c**](https://x.com/p) and [d](https://y.com)");
  EXPECT_EQ(r.rml, "<p>" + LinkSpan(0, "a") + " and " + LinkSpan(0, "b <strong>c</strong>") + " and " +
                       LinkSpan(1, "d") + "</p>");
  // Repeated URLs share one index.
  EXPECT_EQ(r.links, (std::vector<std::string>{"https://x.com/p", "https://y.com"}));

  r = MarkdownToRml("see https://x.com/a_b_c. Also (https://y.com/f(x)) ok");
  EXPECT_EQ(r.rml, "<p>see " + LinkSpan(0, "https://x.com/a_b_c") + ". Also (" + LinkSpan(1, "https://y.com/f(x)") +
                       ") ok</p>");

  r = MarkdownToRml("[https://x.com](https://x.com)");
  EXPECT_EQ(r.rml, "<p>" + LinkSpan(0, "https://x.com") + "</p>");
  EXPECT_EQ(r.links.size(), 1u);
}

TEST(MarkdownToRmlTest, AutolinkInsideChineseText) {
  // Chinese text has no spaces around a URL: it must still link, and stop where the Chinese resumes.
  const auto r = MarkdownToRml("详见https://x.com/a?b=1的内容。");
  EXPECT_EQ(r.rml, "<p>详见" + LinkSpan(0, "https://x.com/a?b=1") + "的内容。</p>");
  EXPECT_EQ(r.links, (std::vector<std::string>{"https://x.com/a?b=1"}));
}

TEST(MarkdownToRmlTest, NonHttpsLinksBecomeText) {
  auto r = MarkdownToRml("[x](http://plain) [y](/rel) [z](ftp://h) [e]()");
  EXPECT_EQ(r.rml, "<p>x (http://plain) y (/rel) z (ftp://h) e</p>");
  EXPECT_TRUE(r.links.empty());
  r = MarkdownToRml("http://plain.example");
  EXPECT_EQ(r.rml, "<p>http://plain.example</p>");
  EXPECT_TRUE(r.links.empty());
}

TEST(MarkdownToRmlTest, ImagesRenderAltOnly) {
  auto r = MarkdownToRml("![a <b> **c**](https://x.com/i.png) end");
  EXPECT_EQ(r.rml, "<p>a &lt;b&gt; c end</p>");
  EXPECT_TRUE(r.links.empty());
  EXPECT_EQ(Rml("![](https://x.com/i.png)"), "<p></p>");
}

TEST(MarkdownToRmlTest, UnterminatedInlineMarkersAreLiteral) {
  EXPECT_EQ(Rml("**a"), "<p>**a</p>");
  EXPECT_EQ(Rml("**a*"), "<p>**a*</p>");
  EXPECT_EQ(Rml("*a"), "<p>*a</p>");
  EXPECT_EQ(Rml("`a"), "<p>`a</p>");
  EXPECT_EQ(Rml("[a](https://x"), "<p>[a](https://x</p>");
  EXPECT_EQ(Rml("[a]("), "<p>[a](</p>");
  EXPECT_EQ(Rml("[a"), "<p>[a</p>");
  EXPECT_EQ(Rml("![a](https://x"), "<p>![a](https://x</p>");
}

TEST(MarkdownToRmlTest, HostileInputIsEscaped) {
  const std::vector<std::string> inputs = {
      "<script>alert(1)</script>",
      "<img src=x onerror=alert(1)>",
      "{{turn.secret}} and {{ x }}",
      "[a\"b<c>](https://x.com)",
      "[x](javascript:alert(1))",
      "[x](http://plain)",
      "javascript:alert(1) ONCLICK=x",
      "<a onclick=\"x\">t</a>",
      "[x](https://a.com/'\"\\)){{q}}",
      "# <b>{{h}}</b>\n- <i>{{l}}</i>\n> <u>{{q}}</u>\n| <a> | b |\n|---|---|\n| {{c}} | <d> |",
      "```\n<script>{{x}}</script>\n```",
      "![<x>{{y}}](https://x.com/a.png)",
  };
  for (const auto& md : inputs) {
    const auto r = MarkdownToRml(md);
    ExpectSafe(r.rml);
    for (const auto& url : r.links) {
      EXPECT_EQ(url.rfind("https://", 0), 0u) << url;
    }
  }
  EXPECT_EQ(Rml("<script>alert(1)</script>"), "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>");
  EXPECT_EQ(Rml("{{turn.secret}}"), "<p>&#123;&#123;turn.secret&#125;&#125;</p>");
  EXPECT_EQ(Rml("[a\"b<c>](https://x.com)"), "<p>" + LinkSpan(0, "a&quot;b&lt;c&gt;") + "</p>");

  const auto plain = MarkdownToRml("[x](javascript:alert(1)) [y](http://plain)");
  EXPECT_TRUE(plain.links.empty());
  EXPECT_EQ(plain.rml.find("<a"), std::string::npos);

  // The URL only ever appears in `links`, never in the markup.
  const auto tricky = MarkdownToRml("[x](https://a.com/'\"\\\\{{q}})");
  ASSERT_EQ(tricky.links.size(), 1u);
  EXPECT_EQ(tricky.links[0], "https://a.com/'\"\\{{q}}");
  EXPECT_EQ(tricky.rml, "<p>" + LinkSpan(0, "x") + "</p>");
}

namespace {

const char* kStreamingDoc =
    "# Release notes \xF0\x9F\x9A\x80\n"
    "\n"
    "Here is **bold**, *em*, `code` and a [link](https://example.com/a?b=1) plus https://example.org/x.\n"
    "\xE4\xBB\x8A\xE5\xA4\xA9\xE7\x9A\x84**\xE9\x87\x8D\xE7\x82\xB9**\xE6\x98\xAF\xE6\xB5\x8B\xE8\xAF\x95\xE3\x80\x82\n"
    "\n"
    "## Steps\n"
    "1. First\n"
    "   - nested *a*\n"
    "   - nested `b`\n"
    "2. Second\n"
    "\n"
    "- x\n"
    "  - y\n"
    "    - z\n"
    "\n"
    "| Name | Value |\n"
    "|:-----|------:|\n"
    "| a | **1** |\n"
    "| b | [c](https://c.example) |\n"
    "\n"
    "> quoted *text*\n"
    "> more\n"
    "\n"
    "```cpp\n"
    "int main() { return 0; } // <tag> {{x}}\n"
    "```\n"
    "\n"
    "---\n"
    "![alt](https://img.example/a.png) \\*escaped\\* snake_case_name\n";

} // namespace

TEST(MarkdownToRmlTest, EveryPrefixIsWellFormed) {
  const std::string doc = kStreamingDoc;
  const auto start = std::chrono::steady_clock::now();
  for (size_t len = 0; len <= doc.size(); ++len) {
    const auto r = MarkdownToRml(std::string_view(doc).substr(0, len));
    ASSERT_TRUE(pbr::RmlValidator::ValidateFragment(r.rml).ok) << "prefix " << len << ": " << r.rml;
    ASSERT_EQ(CheckBalanced(r.rml), "") << "prefix " << len << ": " << r.rml;
    ASSERT_EQ(r.rml.find("{{"), std::string::npos) << "prefix " << len;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  std::cout << "[ INFO ] every-prefix test: " << doc.size() + 1 << " prefixes in " << ms.count() << " ms\n";

  const auto full = MarkdownToRml(doc);
  EXPECT_NE(full.rml.find("<table class=\"chat-table\">"), std::string::npos);
  EXPECT_NE(full.rml.find("<div class=\"code-block\">"), std::string::npos);
  EXPECT_EQ(full.links.size(), 3u);
}

// Random mixes of every marker the converter reacts to. Fixed seed: a failure is reproducible.
TEST(MarkdownToRmlTest, RandomMarkerSoupIsWellFormed) {
  const std::vector<std::string> pieces = {
      "*",  "**", "_",   "__",  "`",    "```",  "~~~", "[",    "]",   "(",      ")",  "![",  "](",
      "\\", "\n", "\n\n", " ",  "  ",   "- ",   "1. ", "> ",   "# ",  "|",      "---", "a",  "b c",
      "<",  ">",  "&",   "\"", "'",    "{{",   "}}",  "你好", "https://x.com/a", "http://x", "javascript:", "onclick=",
  };
  std::mt19937 rng(20260930);
  for (int round = 0; round < 20000; ++round) {
    std::string md;
    const size_t count = 1 + rng() % 40;
    for (size_t k = 0; k < count; ++k) {
      md += pieces[rng() % pieces.size()];
    }
    const auto r = MarkdownToRml(md);
    ASSERT_TRUE(pbr::RmlValidator::ValidateFragment(r.rml).ok) << md << "\n=> " << r.rml;
    ASSERT_EQ(CheckBalanced(r.rml), "") << md << "\n=> " << r.rml;
    ASSERT_EQ(r.rml.find("{{"), std::string::npos) << md;
    for (const std::string& link : r.links) {
      ASSERT_EQ(link.rfind("https://", 0), 0u) << md;
    }
  }
}

TEST(MarkdownToRmlTest, AdversarialInputReturnsPromptly) {
  const auto rep = [](const std::string& unit, size_t total) {
    std::string s;
    while (s.size() < total) {
      s += unit;
    }
    return s;
  };
  const std::vector<std::string> inputs = {
      rep("*", 100000),        rep("[", 100000),      rep("](", 100000),      rep("![", 100000),
      rep("`", 100000),        rep("_", 100000),      rep("*a ", 100000),     rep("[a](", 100000),
      rep("**a *b ", 100000),  rep("`a``b```", 100000), rep("https://", 100000), rep("a*b_c[d](e)", 100000),
      rep("- x\n", 100000),    rep("> x\n", 100000),  rep("| a |\n", 100000), rep("```\n", 100000),
      rep("  - x\n", 100000),  rep("[a](https://x.com/(", 100000),
  };
  for (const auto& md : inputs) {
    const auto start = std::chrono::steady_clock::now();
    const auto r = MarkdownToRml(md);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    EXPECT_LT(ms.count(), 2000) << md.substr(0, 12);
    EXPECT_TRUE(pbr::RmlValidator::ValidateFragment(r.rml).ok);
    EXPECT_EQ(CheckBalanced(r.rml), "") << md.substr(0, 12);
    std::cout << "[ INFO ] adversarial '" << md.substr(0, 8) << "...' " << ms.count() << " ms\n";
  }
}
