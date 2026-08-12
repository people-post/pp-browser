#include "base/ui/ViewCatalog.h"

#include "base/platform/IAssetLocator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

class TempAssetLocator : public pbr::IAssetLocator {
public:
  explicit TempAssetLocator(std::filesystem::path root) : root_(std::move(root)) {}

  std::string Resolve(const std::string& relative) const override {
    return (root_ / relative).string();
  }

private:
  std::filesystem::path root_;
};

class ViewCatalogTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / "pp-browser-view-catalog-test";
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_ / "views", ec);
    locator_ = std::make_unique<TempAssetLocator>(root_);
    pbr::IAssetLocator::SetInstance(locator_.get());
    pbr::ViewCatalog::ClearCache();
  }

  void TearDown() override {
    pbr::ViewCatalog::ClearCache();
    pbr::IAssetLocator::SetInstance(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  void WriteView(const std::string& relative, const std::string& contents) {
    const auto path = root_ / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << contents;
  }

  std::filesystem::path root_;
  std::unique_ptr<TempAssetLocator> locator_;
};

} // namespace

TEST_F(ViewCatalogTest, LoadBodyCachesExpandedBody) {
  WriteView("views/frag.rml", "<span>frag</span>");
  WriteView("views/parent.rml", "<div>{{include:views/frag.rml}}</div>");

  const std::string first = pbr::ViewCatalog::LoadBody("views/parent.rml");
  EXPECT_EQ(first, "<div><span>frag</span></div>");

  WriteView("views/parent.rml", "<div>CHANGED</div>");
  WriteView("views/frag.rml", "<span>CHANGED</span>");
  const std::string second = pbr::ViewCatalog::LoadBody("views/parent.rml");
  EXPECT_EQ(second, first) << "second load should hit cache";
}

TEST_F(ViewCatalogTest, ClearCacheForcesReload) {
  WriteView("views/parent.rml", "<div>v1</div>");
  EXPECT_EQ(pbr::ViewCatalog::LoadBody("views/parent.rml"), "<div>v1</div>");

  WriteView("views/parent.rml", "<div>v2</div>");
  pbr::ViewCatalog::ClearCache();
  EXPECT_EQ(pbr::ViewCatalog::LoadBody("views/parent.rml"), "<div>v2</div>");
}
