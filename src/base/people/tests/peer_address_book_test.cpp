#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerAddressBook.h"

#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/peer/peer_id.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <thread>

namespace pbr {
namespace {

class PeerAddressBookTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> port{41300};
    const int listen_port = port.fetch_add(1);
    Libp2pHostConfig host_config;
    host_config.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(listen_port);
    ASSERT_TRUE(host_.Start(host_config));
    auto peer_id = host_.LocalPeerIdBase58();
    ASSERT_TRUE(peer_id);
    local_peer_id_ = *peer_id;
  }

  void TearDown() override {
    host_.Stop();
  }

  std::string MakeMultiaddr(const std::string& ip, int port) const {
    return "/ip4/" + ip + "/tcp/" + std::to_string(port) + "/p2p/" + local_peer_id_;
  }

  Libp2pHost host_;
  std::string local_peer_id_;
};

TEST_F(PeerAddressBookTest, UpsertAndPreferredMultiaddr) {
  PeerAddressBook book;
  const std::string ma = MakeMultiaddr("203.0.113.9", 4001);

  ASSERT_TRUE(book.Upsert(local_peer_id_, ma, PeerAddrSource::Bootstrap));
  EXPECT_TRUE(book.IsDialable(local_peer_id_));
  ASSERT_TRUE(book.PreferredMultiaddr(local_peer_id_));
  EXPECT_EQ(*book.PreferredMultiaddr(local_peer_id_), ma);
  EXPECT_EQ(book.PeerCount(), 1u);
}

TEST_F(PeerAddressBookTest, RejectsPeerIdMismatch) {
  PeerAddressBook book;
  Libp2pHost host2;
  static std::atomic<int> port{41400};
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port.fetch_add(1));
  ASSERT_TRUE(host2.Start(cfg));
  auto other_id = host2.LocalPeerIdBase58();
  ASSERT_TRUE(other_id);
  const std::string wrong_ma = "/ip4/203.0.113.9/tcp/4001/p2p/" + *other_id;

  EXPECT_FALSE(book.Upsert(local_peer_id_, wrong_ma, PeerAddrSource::Manual));
  EXPECT_FALSE(book.IsDialable(local_peer_id_));
  host2.Stop();
}

TEST_F(PeerAddressBookTest, DialSuccessOutranksBootstrap) {
  PeerAddressBook book;
  const std::string bootstrap_ma = MakeMultiaddr("203.0.113.10", 4001);
  const std::string dial_ma = MakeMultiaddr("198.51.100.4", 4002);

  ASSERT_TRUE(book.Upsert(local_peer_id_, bootstrap_ma, PeerAddrSource::Bootstrap));
  ASSERT_TRUE(book.Upsert(local_peer_id_, dial_ma, PeerAddrSource::DialSuccess));

  ASSERT_TRUE(book.PreferredMultiaddr(local_peer_id_));
  EXPECT_EQ(*book.PreferredMultiaddr(local_peer_id_), dial_ma);
}

TEST_F(PeerAddressBookTest, PruneExpiredRemovesStalePeers) {
  PeerAddressBookConfig config;
  config.default_ttl = std::chrono::milliseconds(1);
  PeerAddressBook book(config);
  const std::string ma = MakeMultiaddr("203.0.113.11", 4001);

  ASSERT_TRUE(book.Upsert(local_peer_id_, ma, PeerAddrSource::Manual));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  book.PruneExpired();

  EXPECT_FALSE(book.IsDialable(local_peer_id_));
  EXPECT_EQ(book.PeerCount(), 0u);
}

TEST_F(PeerAddressBookTest, ResolvePeerInfoBuildsDialTarget) {
  PeerAddressBook book;
  const std::string ma = MakeMultiaddr("203.0.113.12", 4001);

  ASSERT_TRUE(book.Upsert(local_peer_id_, ma, PeerAddrSource::Connection));
  auto info = book.ResolvePeerInfo(local_peer_id_);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->id.toBase58(), local_peer_id_);
  ASSERT_EQ(info->addresses.size(), 1u);
  EXPECT_EQ(info->addresses.front().getStringAddress(), ma);
}

TEST_F(PeerAddressBookTest, SyncFromHostMergesAddressRepository) {
  PeerAddressBook book;
  const std::string ma = MakeMultiaddr("203.0.113.13", 4001);
  auto parsed = libp2p::multi::Multiaddress::create(ma);
  ASSERT_TRUE(parsed);
  auto peer_id = libp2p::peer::PeerId::fromBase58(local_peer_id_);
  ASSERT_TRUE(peer_id);
  (void)host_.GetHost().getPeerRepository().getAddressRepository().upsertAddresses(
      peer_id.value(), std::span<const libp2p::multi::Multiaddress>(std::array{parsed.value()}),
      std::chrono::hours(1));

  book.SyncFromHost(host_, local_peer_id_);
  EXPECT_TRUE(book.IsDialable(local_peer_id_));
}

} // namespace
} // namespace pbr
