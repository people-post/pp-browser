// Contacts between an account's devices (multi-device M023 §7): per-contact registers, the newest wins, a removal
// carries as a tombstone.
#include "lib/people/AccountLocker.h"
#include "lib/people/ContactClock.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

Contact Person(const std::string& id, const std::string& account, const std::string& remark = "") {
  Contact c;
  c.id = id;
  c.local.display_name = remark;
  c.remote.ids.push_back({mesh::ContactIdKind::Account, account, true});
  SyncContactMirrors(c);
  return c;
}

TEST(ContactClockTest, LocalAddsEditsAndRemovalsAreNoticed) {
  ContactClock clock;
  EXPECT_TRUE(clock.Observe({Person("c1", "account:bob")}, 100));
  EXPECT_FALSE(clock.Observe({Person("c1", "account:bob")}, 200)) << "nothing changed";
  Contact renamed = Person("c1", "account:bob", "Bob from work");
  EXPECT_TRUE(clock.Observe({renamed}, 300));
  renamed.remote.nickname = "bobby";  // a directory refresh is not the user's edit
  SyncContactMirrors(renamed);
  EXPECT_FALSE(clock.Observe({renamed}, 350));
  EXPECT_TRUE(clock.Observe({}, 400));
  ASSERT_EQ(clock.Entries().size(), 1u);
  EXPECT_FALSE(clock.Entries()[0].present);
  EXPECT_EQ(clock.Entries()[0].at, 400);
}

TEST(ContactClockTest, ANewerRemoteRegisterWinsAndARemovalCarries) {
  ContactClock phone;
  const std::vector<Contact> on_phone{Person("p1", "account:bob"), Person("p2", "account:carol")};
  phone.Observe(on_phone, 100);

  // The laptop renamed Bob at 200 and removed Carol at 250; it also added Dave.
  ContactClock laptop;
  laptop.Observe({Person("l1", "account:bob"), Person("l2", "account:carol")}, 90);
  const std::vector<Contact> on_laptop{Person("l1", "account:bob", "Bobby"), Person("l3", "account:dave")};
  laptop.Observe(on_laptop, 250);

  const ContactClock::Plan plan = phone.MergeRemote(laptop.Entries(), on_laptop, on_phone);
  ASSERT_EQ(plan.remove_contact_ids, std::vector<std::string>{"p2"}) << "Carol, removed on the laptop";
  ASSERT_EQ(plan.upsert.size(), 2u);
  EXPECT_EQ(plan.upsert[0].id, "p1") << "this device's row for Bob";
  EXPECT_EQ(plan.upsert[0].local.display_name, "Bobby") << "with the laptop's remark";
  EXPECT_EQ(ContactClockAccount(plan.upsert[1]), "account:dave");

  // Applied, the phone sees nothing new of its own: no echo back as a fresh edit.
  const std::vector<Contact> after{plan.upsert[0], Person("p3", "account:dave")};
  EXPECT_FALSE(phone.Observe(after, 300));

  // An older remote register loses (ours is newer).
  ContactClock stale;
  stale.Observe({Person("s1", "account:bob", "Old name")}, 50);
  EXPECT_TRUE(phone.MergeRemote(stale.Entries(), {Person("s1", "account:bob", "Old name")}, after).empty());
}

TEST(ContactClockTest, TheLockerMergeCarriesARemovalByItsClock) {
  ContactClock laptop;
  laptop.Observe({Person("l1", "account:bob"), Person("l2", "account:carol")}, 100);
  laptop.Observe({Person("l1", "account:bob")}, 200);  // Carol removed
  AccountLockerState stored;
  stored.contacts = {Person("l1", "account:bob")};
  stored.contact_clock = laptop.Entries();

  ContactClock phone;  // still has Carol, from before
  phone.Observe({Person("p1", "account:bob"), Person("p2", "account:carol")}, 100);
  AccountLockerState ours;
  ours.contacts = {Person("p1", "account:bob"), Person("p2", "account:carol")};
  ours.contact_clock = phone.Entries();

  const AccountLockerState merged = account_locker::MergeForPublish(ours, stored);
  ASSERT_EQ(merged.contacts.size(), 1u) << "Carol stays removed";
  EXPECT_EQ(ContactClockAccount(merged.contacts[0]), "account:bob");
  EXPECT_EQ(merged.contact_clock.size(), 2u) << "the tombstone travels on";
}

} // namespace
} // namespace pbr
