#include "lib/net/RelayOrigins.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Our relays (multi-relay R2, docs/ops/CONFIGURATION.md `relay.extra_base_urls`): the backend's first, then the
// configured others in order, at most four in all (MR004), no relay twice.

using Origins = std::vector<std::string>;

TEST(RelayOriginsTest, ThePreferredRelayComesFirstThenTheOthersInOrder) {
  EXPECT_EQ(
      OurRelayOrigins("https://a.example/api/relay", {"https://b.example/api/relay", "https://c.example/api/relay"}),
      Origins({"https://a.example/api/relay", "https://b.example/api/relay", "https://c.example/api/relay"}));
}

TEST(RelayOriginsTest, AtMostFourRelaysInAll) {
  EXPECT_EQ(OurRelayOrigins("https://a.example",
                            {"https://b.example", "https://c.example", "https://d.example", "https://e.example"}),
            Origins({"https://a.example", "https://b.example", "https://c.example", "https://d.example"}));
}

TEST(RelayOriginsTest, ARelayListedTwiceOrNotAUrlIsLeftOut) {
  EXPECT_EQ(OurRelayOrigins("https://a.example/api/relay",
                            {"https://A.example:443/api/relay/", "relay.example", "", "ftp://x.example",
                             "https:///no-host", "https://q.example/?x=1", " https://b.example/api/relay/ "}),
            Origins({"https://a.example/api/relay", "https://b.example/api/relay"}));
}

// The contact page names a relay by its host (multi-relay R3), not by its URL.
TEST(RelayOriginsTest, ARelayIsNamedByItsHost) {
  EXPECT_EQ(RelayOriginHost("https://www.Brief.global/api/relay"), "www.brief.global");
  EXPECT_EQ(RelayOriginHost("https://relay.example:443/"), "relay.example");
  EXPECT_EQ(RelayOriginHost("https://relay.example:8443/api/relay"), "relay.example:8443");
  EXPECT_EQ(RelayOriginHost("http://10.0.0.2:8080/api/relay"), "10.0.0.2:8080");
  EXPECT_EQ(RelayOriginHost("relay.example"), "");
}

TEST(RelayOriginsTest, NoPreferredRelayMeansNone) {
  EXPECT_TRUE(OurRelayOrigins("", {"https://b.example"}).empty());
}

TEST(RelayOriginsTest, TheSameRelayWrittenDifferently) {
  EXPECT_TRUE(SameRelay("https://Relay.Example:443/api/relay/", "https://relay.example/api/relay"));
  EXPECT_TRUE(SameRelay("http://127.0.0.1:80/api/relay", "http://127.0.0.1/api/relay"));
  EXPECT_FALSE(SameRelay("https://relay.example:8443/api/relay", "https://relay.example/api/relay"));
  EXPECT_FALSE(SameRelay("http://relay.example/api/relay", "https://relay.example/api/relay"));
  EXPECT_FALSE(SameRelay("https://relay.example/api/Relay", "https://relay.example/api/relay")) << "paths keep case";
}

// A relay named by a contact's card, a QR code or a person is held to a public https host.
TEST(PublicRelayOriginTest, APublicHttpsOriginIsAllowed) {
  for (const char* origin : {"https://relay.example",
                             "https://relay.example/api/relay",
                             "https://RELAY.Example:8443/x",
                             "HTTPS://relay.example",
                             "https://relay.example.:443/",
                             "https://sub.relay.example",
                             "https://8.8.8.8",
                             "https://8.8.8.8:8443/api",
                             "https://[2606:4700:4700::1111]/",
                             "https://[2606:4700:4700::1111]:8443/x",
                             "https://relay.example.com.cn",
                             "https://www.brief.global/api/relay",
                             "https://172.32.0.1",
                             "https://192.169.0.1",
                             "https://100.128.0.1",
                             "https://localhost.example/api/relay",
                             "https://deadbeef.example",
                             "https://[::ffff:8.8.8.8]",
                             "https://[2606:4700::1111]",
                             "https://1.1.1.1"}) {
    EXPECT_TRUE(IsPublicRelayOrigin(origin)) << origin;
  }
}

TEST(PublicRelayOriginTest, AnotherSchemeOrUserinfoIsRefused) {
  for (const char* origin :
       {"http://relay.example", "ftp://relay.example", "relay.example", "//relay.example", "https://", "https:///x",
        "https://user@relay.example", "https://user:pw@relay.example", "https://relay.example@evil.example",
        "https://relay.example\\@8.8.8.8", "https://relay .example", ""}) {
    EXPECT_FALSE(IsPublicRelayOrigin(origin)) << origin;
  }
}

TEST(PublicRelayOriginTest, AnInternalIpv4LiteralIsRefused) {
  for (const char* origin : {"https://127.0.0.1", "https://127.1.2.3:8443/", "https://10.0.0.1", "https://172.16.0.1",
                             "https://172.31.255.255", "https://192.168.1.1", "https://169.254.169.254/latest",
                             "https://0.0.0.0", "https://0.1.2.3", "https://100.64.0.1", "https://100.127.255.255",
                             "https://127.0.0.1.", "https://224.0.0.1", "https://255.255.255.255"}) {
    EXPECT_FALSE(IsPublicRelayOrigin(origin)) << origin;
  }
}

TEST(PublicRelayOriginTest, AnInternalIpv6LiteralIsRefused) {
  for (const char* origin : {"https://[::1]",
                             "https://[::]",
                             "https://[0:0:0:0:0:0:0:1]/",
                             "https://[fc00::1]",
                             "https://[fd12:3456::1]",
                             "https://[fe80::1]",
                             "https://[febf::1]:8443",
                             "https://[::ffff:127.0.0.1]",
                             "https://[::ffff:10.0.0.1]/",
                             "https://[::ffff:7f00:1]",
                             "https://[::ffff:a9fe:a9fe]",
                             "https://[::ffff:c0a8:101]",
                             "https://[FE80::1]",
                             "https://[fe80::1%25eth0]",
                             "https://[not-an-address]",
                             "https://[]",
                             "https://[::1",
                             "https://[1::2::3]",
                             "https://[12345::1]",
                             "https://[1:2:3:4:5:6:7:8:9]",
                             "https://[::ffff:127.1]",
                             "https://[::ffff:8.8.8.08]"}) {
    EXPECT_FALSE(IsPublicRelayOrigin(origin)) << origin;
  }
}

TEST(PublicRelayOriginTest, AnInternalNameIsRefused) {
  for (const char* origin :
       {"https://localhost", "https://LOCALHOST:8443", "https://localhost.", "https://app.localhost",
        "https://a.b.localhost.", "https://printer.local", "https://Printer.LOCAL.", "https://db.internal",
        "https://svc.cluster.internal:443/x", "https://intranet", "https://relay", "https://relay:8443",
        "https://2130706433", "https://0x7f.1", "https://127.1", "https://0177.0.0.1", "https://relay.example.1"}) {
    EXPECT_FALSE(IsPublicRelayOrigin(origin)) << origin;
  }
}

TEST(PublicRelayOriginTest, ALabBuildTakesAnyHttpOrigin) {
  for (const char* origin : {"https://127.0.0.1:8443", "http://relay.lab", "https://[::1]/", "https://localhost"}) {
    EXPECT_TRUE(IsPublicRelayOrigin(origin, true)) << origin;
  }
  EXPECT_FALSE(IsPublicRelayOrigin("ftp://relay.lab", true));
  EXPECT_FALSE(IsPublicRelayOrigin("https://user@relay.lab", true));
}

} // namespace
} // namespace pbr
