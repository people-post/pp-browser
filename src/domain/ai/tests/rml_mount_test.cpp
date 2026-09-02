#include "domain/ai/RmlValidator.h"

#include <gtest/gtest.h>

TEST(RmlMountTest, ValidatesFragmentsAndDocuments) {
  const std::string valid_fragment =
      R"frag(<div class="stack"><button data-event-click="go()">Go</button></div>)frag";
  auto ok = pbr::RmlValidator::ValidateFragment(valid_fragment);
  EXPECT_TRUE(ok.ok);

  const std::string bad_fragment = R"bad(<div onclick="alert(1)"></div>)bad";
  auto bad_result = pbr::RmlValidator::ValidateFragment(bad_fragment);
  EXPECT_FALSE(bad_result.ok);

  const std::string document_without_root = "<body><p>Hi</p></body>";
  auto doc_result = pbr::RmlValidator::ValidateRml(document_without_root);
  EXPECT_FALSE(doc_result.ok);

  const std::string valid_document = "<rml><body><p>Hi</p></body></rml>";
  EXPECT_TRUE(pbr::RmlValidator::ValidateRml(valid_document).ok);
}
