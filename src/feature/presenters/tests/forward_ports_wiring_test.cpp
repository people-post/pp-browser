// Every port of a forward that the messaging layer provides must be wired in the presenter's FacadeForwardPorts: a
// port added to ForwardPorts and forgotten there is unset in the real app (the tests' fakes set everything), and a
// missing safety port once meant duplicated pictures after a server 429. This reads the sources, so it fails the day a
// port is declared and not assigned.
#include <gtest/gtest.h>

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
  return out.str();
}

/** The names of the std::function members of `struct ForwardPorts { ... };`. */
std::set<std::string> DeclaredPorts(const std::string& header) {
  const size_t begin = header.find("struct ForwardPorts {");
  const size_t end = header.find("\n};", begin);
  EXPECT_NE(begin, std::string::npos);
  EXPECT_NE(end, std::string::npos);
  std::string body = header.substr(begin, end - begin);
  body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), " "); // comments
  body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), " ");
  std::set<std::string> names;
  // "std::function<...> name;" - the type may span lines and holds nested <> and ().
  const std::regex member(R"(>\s*([a-z_][a-z_0-9]*)\s*;)");
  for (auto it = std::sregex_iterator(body.begin(), body.end(), member); it != std::sregex_iterator(); ++it) {
    names.insert((*it)[1]);
  }
  return names;
}

TEST(ForwardPortsWiringTest, EveryForwardPortIsWiredFromTheFacade) {
  const std::string root = PP_SOURCE_DIR;
  const std::string header = ReadAll(root + "/src/feature/presenters/MessageForward.h");
  std::string source = ReadAll(root + "/src/feature/presenters/ChatPresenter.cpp");
  ASSERT_FALSE(header.empty());
  ASSERT_FALSE(source.empty());
  const size_t begin = source.find("ForwardPorts FacadeForwardPorts(ConversationsFacade* facade) {");
  ASSERT_NE(begin, std::string::npos);
  const size_t end = source.find("\n}\n", begin);
  ASSERT_NE(end, std::string::npos);
  const std::string wiring = source.substr(begin, end - begin);

  const std::set<std::string> declared = DeclaredPorts(header);
  ASSERT_GE(declared.size(), 15u) << "the parse found the struct's members";
  // Not from the facade, on purpose: the presenter's own switch (SetForwardAttachmentBusy / ChatController).
  const std::set<std::string> elsewhere = {"attachment_busy"};
  for (const std::string& name : declared) {
    if (elsewhere.count(name) > 0) {
      continue;
    }
    EXPECT_NE(wiring.find("ports." + name + " ="), std::string::npos)
        << "ForwardPorts::" << name << " is declared but FacadeForwardPorts never sets it";
  }
  for (const std::string& name : elsewhere) {
    EXPECT_EQ(declared.count(name), 1u) << name << " is no longer a port: drop it from the exemption";
  }
}

} // namespace
} // namespace pbr
