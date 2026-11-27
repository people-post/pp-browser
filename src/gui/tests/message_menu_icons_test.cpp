// The message menu's icons are one family: Forward and Select are outline icons drawn with the same stroke as Reply,
// and Forward is Reply flipped left to right (assets/icons/v2/README.md).
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string Icon(const char* name) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/icons/v2/" + name + ".svg", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The value of an attribute of the <svg> root, e.g. viewBox="0 0 24 24" gives "0 0 24 24"; empty when absent.
std::string RootAttr(const std::string& svg, const std::string& name) {
  std::smatch m;
  const std::regex re("<svg[^>]*\\s" + name + "=\"([^\"]*)\"");
  return std::regex_search(svg, m, re) ? m[1].str() : std::string();
}

std::vector<std::string> PathData(const std::string& svg) {
  std::vector<std::string> out;
  const std::regex re("<path d=\"([^\"]*)\"");
  for (auto it = std::sregex_iterator(svg.begin(), svg.end(), re); it != std::sregex_iterator(); ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

struct Segment {
  char command;
  std::vector<double> args;
  bool operator==(const Segment& other) const { return command == other.command && args == other.args; }
};

std::vector<Segment> Parse(const std::string& d) {
  std::vector<Segment> out;
  const std::regex token("[A-Za-z]|-?[0-9]*\\.?[0-9]+");
  for (auto it = std::sregex_iterator(d.begin(), d.end(), token); it != std::sregex_iterator(); ++it) {
    const std::string t = (*it)[0].str();
    if (std::isalpha(static_cast<unsigned char>(t[0]))) {
      out.push_back({t[0], {}});
    } else {
      out.back().args.push_back(std::atof(t.c_str()));
    }
  }
  return out;
}

// Reply's path flipped left to right inside the 24-wide canvas: absolute x becomes 24 - x, a relative horizontal move
// changes sign, an arc's sweep flag toggles. Only the commands Reply uses (M, L, H, v, a) are handled.
std::vector<Segment> MirrorX(std::vector<Segment> segments) {
  for (Segment& s : segments) {
    switch (s.command) {
      case 'M':
      case 'L':
        for (size_t i = 0; i + 1 < s.args.size(); i += 2) {
          s.args[i] = 24 - s.args[i];
        }
        break;
      case 'H':
        s.args[0] = 24 - s.args[0];
        break;
      case 'a':
        s.args[4] = 1 - s.args[4];
        s.args[5] = -s.args[5];
        break;
      case 'v':
        break;
      default:
        ADD_FAILURE() << "unexpected command " << s.command;
    }
  }
  return segments;
}

TEST(MessageMenuIconsTest, ForwardAndSelectUseOutlineIconsOfTheSameStrokeAsReply) {
  const std::string reply = Icon("reply");
  ASSERT_FALSE(reply.empty());
  for (const char* name : {"forward", "select"}) {
    SCOPED_TRACE(name);
    const std::string svg = Icon(name);
    ASSERT_FALSE(svg.empty());
    for (const char* attr :
         {"viewBox", "width", "height", "fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin"}) {
      EXPECT_EQ(RootAttr(svg, attr), RootAttr(reply, attr)) << attr;
    }
    EXPECT_EQ(RootAttr(svg, "stroke-width"), "1.75");
    EXPECT_EQ(RootAttr(svg, "fill"), "none");
    // No shape carries its own fill or stroke override: Reply has none either.
    EXPECT_EQ(svg.find("fill=\"#"), std::string::npos);
    std::smatch m;
    EXPECT_FALSE(std::regex_search(svg, m, std::regex("<(path|circle|rect)[^>]*(fill|stroke)")));
  }
}

TEST(MessageMenuIconsTest, ForwardIsTheMirrorOfReply) {
  const std::vector<std::string> reply = PathData(Icon("reply"));
  const std::vector<std::string> forward = PathData(Icon("forward"));
  ASSERT_EQ(reply.size(), 2u);
  ASSERT_EQ(forward.size(), reply.size());
  for (size_t i = 0; i < reply.size(); ++i) {
    EXPECT_EQ(Parse(forward[i]), MirrorX(Parse(reply[i]))) << forward[i];
  }
}

TEST(MessageMenuIconsTest, TheMessageMenuUsesThem) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string source = out.str();
  const size_t forward = source.find("\"forward_message\"");
  const size_t select = source.find("\"select_messages\"");
  ASSERT_NE(forward, std::string::npos);
  ASSERT_NE(select, std::string::npos);
  EXPECT_NE(source.find("../icons/v2/forward.svg", forward), std::string::npos);
  EXPECT_LT(source.find("../icons/v2/forward.svg", forward), select);
  EXPECT_NE(source.find("../icons/v2/select.svg", select), std::string::npos);
}

TEST(MessageMenuIconsTest, TheDesktopHoverBarForwardButtonUsesTheForwardIcon) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/views/chat_wide.rml", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string view = out.str();
  const size_t button = view.find("msg_bar_forward()");
  ASSERT_NE(button, std::string::npos);
  const size_t close = view.find("</button>", button);
  const size_t icon = view.find("../icons/v2/forward.svg", button);
  EXPECT_LT(icon, close);
  EXPECT_EQ(view.find("../icons/v2/share.svg"), std::string::npos);
}

} // namespace
} // namespace pbr
