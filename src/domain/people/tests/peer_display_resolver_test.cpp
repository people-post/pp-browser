// The name a thread or a sender shows (contacts here; the directory cache and the group roster through ports).
#include "domain/people/PeerDisplayResolver.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>

namespace pbr {
namespace {

class PeerDisplayResolverTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_browser_peer_resolver_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    PeerDisplaySources sources;
    sources.directory = [this](const std::string& id) -> std::optional<mesh::DirectoryHit> {
      auto it = directory_.find(id);
      return it == directory_.end() ? std::nullopt : std::optional<mesh::DirectoryHit>(it->second);
    };
    sources.group_title = [this](const std::string& group_id) -> std::optional<std::string> {
      auto it = groups_.find(group_id);
      return it == groups_.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
    resolver_ = std::make_unique<PeerDisplayResolver>(*contacts_, std::move(sources));
  }
  void TearDown() override {
    resolver_.reset();
    contacts_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<ContactsStore> contacts_;
  std::map<std::string, mesh::DirectoryHit> directory_;
  std::map<std::string, std::string> groups_;
  std::unique_ptr<PeerDisplayResolver> resolver_;
};

TEST_F(PeerDisplayResolverTest, PrefersContactThenDirectoryThenShortId) {
  Thread thread;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_kind = "account";
  thread.peer_identity_value = "account:lHaEnkO4vSbVnl6A";
  thread.title = thread.peer_identity_value;

  PeerDisplayLabel raw = resolver_->ResolveThread(thread);
  EXPECT_EQ(raw.trust, PeerLabelTrust::RawId);
  EXPECT_EQ(raw.title, "account:lHaE…");

  mesh::DirectoryHit hit;
  hit.nickname = "alice";
  hit.account_id = thread.peer_identity_value;
  hit.ids = {{mesh::ContactIdKind::Account, thread.peer_identity_value, true},
             {mesh::ContactIdKind::RelayUser, "relay:lHaEnkO4vSbVnl6A", false}};
  directory_[thread.peer_identity_value] = hit;
  PeerDisplayLabel unverified = resolver_->ResolveThread(thread);
  EXPECT_EQ(unverified.trust, PeerLabelTrust::DirectoryUnverified);
  EXPECT_EQ(unverified.title, "~alice @account:lHaE…");

  Contact contact;
  contact.id = "c1";
  contact.display_name = "Alice Example";
  contact.server_nickname = "alice";
  contact.ids = hit.ids;
  ASSERT_TRUE(contacts_->Upsert(contact));
  PeerDisplayLabel trusted = resolver_->ResolveThread(thread);
  EXPECT_EQ(trusted.trust, PeerLabelTrust::Contact);
  EXPECT_EQ(trusted.title, "Alice Example");
  EXPECT_EQ(trusted.contact_id, std::optional<std::string>("c1"));
}

TEST_F(PeerDisplayResolverTest, GroupLocalTitleWinsOverShared) {
  groups_["group:1"] = "Hiking Crew";
  Thread thread;
  thread.kind = ThreadKind::Group;
  thread.group_id = "group:1";
  thread.title = "Old title";
  thread.local_title = "Weekend hike";
  PeerDisplayLabel label = resolver_->ResolveThread(thread);
  EXPECT_EQ(label.title, "Weekend hike");
  EXPECT_EQ(label.shared_title, std::optional<std::string>("Hiking Crew")) << "the roster's title is the shared one";

  thread.local_title.clear();
  label = resolver_->ResolveThread(thread);
  EXPECT_EQ(label.title, "Hiking Crew");
  EXPECT_FALSE(label.shared_title);

  groups_.clear();
  EXPECT_EQ(resolver_->ResolveThread(thread).title, "Old title") << "no roster title: the stored one";
}

} // namespace
} // namespace pbr
