#include "lib/delivery/OurRelaysDirectory.h"

#include "lib/people/PeopleLookupFailure.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

// The directory as our relays answer it (multi-relay R2; DESIGN "A name search asks the relays the user has listed"):
// a search asks every relay of ours at once and merges, a lookup asks them in order; a relay down fails neither.

mesh::DirectoryHit Entry(const std::string& account_id, const std::string& nickname) {
  mesh::DirectoryHit hit;
  hit.hit_id = account_id;
  hit.account_id = account_id;
  hit.nickname = nickname;
  return hit;
}

/** One relay's directory: its entries, or down; a slow one answers only once released. */
class FakeDirectory : public mesh::IDirectoryClient {
public:
  explicit FakeDirectory(std::vector<mesh::DirectoryHit> entries) : entries_(std::move(entries)) {}

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    WaitIfSlow();
    std::lock_guard lock(mutex_);
    ++searches_;
    if (down_) {
      return Error("Directory search failed with status 503");
    }
    return entries_;
  }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override {
    std::lock_guard lock(mutex_);
    looked_up_.push_back(account_id);
    if (down_) {
      return Error("Directory lookup failed with status 503");
    }
    for (const mesh::DirectoryHit& hit : entries_) {
      if (hit.account_id == account_id) {
        return hit;
      }
    }
    return Error("Account not found");
  }
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override {
    mesh::MeshNodeHit node;
    node.nickname = entries_.empty() ? "" : entries_.front().nickname;
    return std::vector<mesh::MeshNodeHit>{node};
  }

  void SetDown(bool down) {
    std::lock_guard lock(mutex_);
    down_ = down;
  }
  void SetSlow() {
    std::lock_guard lock(mutex_);
    slow_ = true;
  }
  void Release() {
    std::lock_guard lock(mutex_);
    slow_ = false;
    released_.notify_all();
  }
  int Searches() {
    std::lock_guard lock(mutex_);
    return searches_;
  }
  std::vector<std::string> LookedUp() {
    std::lock_guard lock(mutex_);
    return looked_up_;
  }

private:
  void WaitIfSlow() {
    std::unique_lock lock(mutex_);
    released_.wait_for(lock, std::chrono::seconds(10), [this] { return !slow_; });
  }

  std::vector<mesh::DirectoryHit> entries_;
  std::mutex mutex_;
  std::condition_variable released_;
  bool down_ = false;
  bool slow_ = false;
  int searches_ = 0;
  std::vector<std::string> looked_up_;
};

class OurRelaysDirectoryTest : public ::testing::Test {
protected:
  void TearDown() override {
    a_->Release();
    b_->Release();
    c_->Release();
    for (std::thread& thread : threads_) {
      thread.join();
    }
  }

  /** The directory over a, b and c; with `elsewhere`, the others are asked on threads of their own. */
  std::unique_ptr<OurRelaysDirectory> Make(bool elsewhere, int64_t timeout_ms = OurRelaysDirectory::kAnswerTimeoutMs) {
    OurRelaysDirectory::RunElsewhere run;
    if (elsewhere) {
      run = [this](std::function<void()> task) { threads_.emplace_back(std::move(task)); };
    }
    return std::make_unique<OurRelaysDirectory>(
        std::vector<OurRelaysDirectory::Source>{{.origin = "https://a.example/api/relay", .client = a_},
                                                {.origin = "https://b.example/api/relay", .client = b_},
                                                {.origin = "https://c.example/api/relay", .client = c_}},
        std::move(run), timeout_ms);
  }

  static std::vector<std::string> Nicknames(const std::vector<mesh::DirectoryHit>& hits) {
    std::vector<std::string> out;
    for (const mesh::DirectoryHit& hit : hits) {
      out.push_back(hit.nickname);
    }
    return out;
  }

  std::shared_ptr<FakeDirectory> a_ =
      std::make_shared<FakeDirectory>(std::vector{Entry("account:bob", "bob"), Entry("account:ann", "ann")});
  std::shared_ptr<FakeDirectory> b_ =
      std::make_shared<FakeDirectory>(std::vector{Entry("account:cat", "cat"), Entry("account:bob", "bob")});
  std::shared_ptr<FakeDirectory> c_ = std::make_shared<FakeDirectory>(std::vector{Entry("account:dan", "dan")});
  std::vector<std::thread> threads_;
};

TEST_F(OurRelaysDirectoryTest, ASearchAsksEveryRelayAndMergesThePreferredFirst) {
  for (const bool elsewhere : {false, true}) {
    auto directory = Make(elsewhere);
    auto hits = directory->SearchPeople("b");
    ASSERT_TRUE(hits) << hits.error().message;
    EXPECT_EQ(Nicknames(*hits), (std::vector<std::string>{"bob", "ann", "cat", "dan"}))
        << "each account once; the preferred relay's order, then the others' new ones";
  }
}

TEST_F(OurRelaysDirectoryTest, ARelayDownFailsNothingAndOnlyAllDownFailTheSearch) {
  auto directory = Make(true);
  b_->SetDown(true);
  auto hits = directory->SearchPeople("b");
  ASSERT_TRUE(hits);
  EXPECT_EQ(Nicknames(*hits), (std::vector<std::string>{"bob", "ann", "dan"}));

  a_->SetDown(true);
  hits = directory->SearchPeople("b");
  ASSERT_TRUE(hits) << "the preferred relay down: the others still answer";
  EXPECT_EQ(Nicknames(*hits), (std::vector<std::string>{"dan"}));

  c_->SetDown(true);
  hits = directory->SearchPeople("b");
  ASSERT_FALSE(hits);
  EXPECT_EQ(ClassifyPeopleLookupError(hits.error()), PeopleLookupFailure::DirectoryUnreachable)
      << "none answered: the preferred one's failure, as with one relay";
}

TEST_F(OurRelaysDirectoryTest, ARelayThatDoesNotAnswerInTimeIsLeftOut) {
  auto directory = Make(true, 300);
  c_->SetSlow();
  const auto started = std::chrono::steady_clock::now();
  auto hits = directory->SearchPeople("b");
  const auto took = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(hits);
  EXPECT_EQ(Nicknames(*hits), (std::vector<std::string>{"bob", "ann", "cat"}));
  EXPECT_LT(took, std::chrono::seconds(5)) << "the search does not wait for the slow relay";
  c_->Release(); // its late answer lands after the search and is dropped
}

TEST_F(OurRelaysDirectoryTest, ALookupAsksInOrderAndStopsAtTheFirstThatKnowsTheAccount) {
  auto directory = Make(false);
  auto hit = directory->LookupByAccount("account:bob");
  ASSERT_TRUE(hit);
  EXPECT_EQ(b_->LookedUp().size(), 0u) << "the preferred relay knew it";

  hit = directory->LookupByAccount("account:dan");
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->nickname, "dan");
  EXPECT_EQ(b_->LookedUp(), (std::vector<std::string>{"account:dan"})) << "asked in order";
}

TEST_F(OurRelaysDirectoryTest, ALookupNotFoundWhereItCouldAskIsNotFoundAndARelayDownIsAskedLast) {
  auto directory = Make(false);
  b_->SetDown(true);
  auto hit = directory->LookupByAccount("account:nobody");
  ASSERT_FALSE(hit);
  EXPECT_EQ(ClassifyPeopleLookupError(hit.error()), PeopleLookupFailure::NotFound)
      << "a relay down does not turn not-found into a failure";

  hit = directory->LookupByAccount("account:dan");
  ASSERT_TRUE(hit);
  EXPECT_EQ(b_->LookedUp().size(), 1u) << "the relay that just failed is asked after the others, which knew it";

  a_->SetDown(true);
  c_->SetDown(true);
  hit = directory->LookupByAccount("account:dan");
  ASSERT_FALSE(hit);
  EXPECT_EQ(ClassifyPeopleLookupError(hit.error()), PeopleLookupFailure::DirectoryUnreachable) << "reached none";
}

TEST_F(OurRelaysDirectoryTest, MeshNodesAreTheFirstDirectorys) {
  auto directory = Make(false);
  auto nodes = directory->ListMeshNodes();
  ASSERT_TRUE(nodes);
  ASSERT_EQ(nodes->size(), 1u);
  EXPECT_EQ(nodes->front().nickname.value_or(""), "bob");
}

} // namespace
} // namespace pbr
