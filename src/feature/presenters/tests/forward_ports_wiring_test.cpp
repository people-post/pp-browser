// Every port of a forward that the messaging layer provides must be wired in the presenter's FacadeForwardPorts: a
// port added to ForwardPorts and forgotten there is unset in the real app (the tests' fakes set everything), and a
// missing safety port once meant duplicated pictures after a server 429. This reads the sources, so it fails the day a
// port is declared and not assigned.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::erase(text, '\r'); // a Windows checkout has CRLF line ends
  return text;
}

// Comments out, string and character literals kept as they are (a "/*" or "//" inside one is not a comment). Raw string
// literals are not understood: the files read here have none.
std::string StripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      const size_t start = i++;
      while (i < text.size() && text[i] != c && text[i] != '\n') {
        i += text[i] == '\\' ? 2 : 1;
      }
      i = std::min(i + 1, text.size());
      out.append(text, start, i - start);
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const size_t end = text.find("*/", i + 2);
      i = end == std::string::npos ? text.size() : end + 2;
      out += ' ';
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
      out += ' ';
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

/** The names of every member of `struct ForwardPorts { ... };`, whatever its type. */
std::set<std::string> DeclaredPorts(const std::string& header) {
  const std::string text = StripComments(header);
  const size_t begin = text.find("struct ForwardPorts {");
  EXPECT_NE(begin, std::string::npos);
  size_t at = text.find('{', begin) + 1;
  std::set<std::string> names;
  std::string statement;
  int depth = 0; // (), <> and {} inside a member's type or initialiser
  for (; at < text.size(); ++at) {
    const char c = text[at];
    if (c == '(' || c == '<' || c == '{') {
      ++depth;
    } else if (c == ')' || c == '>') {
      --depth;
    } else if (c == '}') {
      if (depth == 0) {
        break; // the struct's end
      }
      --depth;
    }
    if (c == ';' && depth == 0) {
      const size_t equals = statement.find('=');
      const std::string head = equals == std::string::npos ? statement : statement.substr(0, equals);
      const std::regex name(R"(([A-Za-z_][A-Za-z_0-9]*)\s*$)");
      std::smatch m;
      if (std::regex_search(head, m, name)) {
        names.insert(m[1]);
      }
      statement.clear();
    } else {
      statement += c;
    }
  }
  return names;
}

TEST(ForwardPortsWiringTest, EveryForwardPortIsWiredFromTheFacade) {
  const std::string root = PP_SOURCE_DIR;
  const std::string header = ReadAll(root + "/src/feature/presenters/MessageForward.h");
  const std::string source = StripComments(ReadAll(root + "/src/feature/presenters/ChatPresenter.cpp"));
  ASSERT_FALSE(header.empty());
  ASSERT_FALSE(source.empty());
  const size_t begin = source.find("ForwardPorts FacadeForwardPorts(ConversationsFacade* facade) {");
  ASSERT_NE(begin, std::string::npos);
  const size_t end = source.find("\n}\n", begin);
  ASSERT_NE(end, std::string::npos);
  const std::string wiring = source.substr(begin, end - begin);

  const std::set<std::string> declared = DeclaredPorts(header);
  ASSERT_GE(declared.size(), 15u) << "the parse found the struct's members";
  EXPECT_EQ(declared.count("staged_attachments"), 1u);
  EXPECT_EQ(declared.count("now_ms"), 1u);
  for (const std::string& name : declared) {
    EXPECT_NE(wiring.find("ports." + name + " ="), std::string::npos)
        << "ForwardPorts::" << name << " is declared but FacadeForwardPorts never sets it";
  }
}

} // namespace
} // namespace pbr
