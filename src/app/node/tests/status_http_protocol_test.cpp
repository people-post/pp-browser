#include "app/node/StatusHttpProtocol.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

TEST(StatusHttpProtocolTest, ParsesDefaultBindAndBarePort) {
  auto def = pbr::ParseStatusHttpBind("127.0.0.1:18518");
  ASSERT_TRUE(def);
  EXPECT_EQ(def->host, "127.0.0.1");
  EXPECT_EQ(def->port, 18518);

  auto bare = pbr::ParseStatusHttpBind("9090");
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->host, "127.0.0.1");
  EXPECT_EQ(bare->port, 9090);

  EXPECT_FALSE(pbr::ParseStatusHttpBind(""));
  EXPECT_FALSE(pbr::ParseStatusHttpBind("   "));
}

TEST(StatusHttpProtocolTest, ParsesIpv6Loopback) {
  auto bind = pbr::ParseStatusHttpBind("[::1]:18518");
  ASSERT_TRUE(bind);
  EXPECT_EQ(bind->host, "::1");
  EXPECT_EQ(bind->port, 18518);
  EXPECT_TRUE(pbr::IsLoopbackStatusBindHost(bind->host));
}

TEST(StatusHttpProtocolTest, LoopbackDetection) {
  EXPECT_TRUE(pbr::IsLoopbackStatusBindHost("127.0.0.1"));
  EXPECT_TRUE(pbr::IsLoopbackStatusBindHost("LOCALHOST"));
  EXPECT_TRUE(pbr::IsLoopbackStatusBindHost("::1"));
  EXPECT_FALSE(pbr::IsLoopbackStatusBindHost("0.0.0.0"));
  EXPECT_FALSE(pbr::IsLoopbackStatusBindHost("192.168.1.1"));
}

TEST(StatusHttpProtocolTest, HealthzAndStatus) {
  pbr::StatusHttpSnapshot snap;
  snap.host_running = true;
  snap.listen_multiaddr = "/ip4/0.0.0.0/tcp/443";
  snap.peer_id = "12D3KooWtest";
  snap.circuit_relay = true;
  snap.media_relay = false;
  snap.reachability_json = R"({"status":"reachable","seed_dial_ok":true})";

  pbr::StatusHttpRequest health{.method = "GET", .path = "/healthz", .authorization = {}};
  auto h = pbr::HandleStatusHttpRequest(health, {}, snap);
  EXPECT_EQ(h.status_code, 200);
  auto hj = pbr::TryParseObject(h.body);
  ASSERT_TRUE(hj);
  EXPECT_TRUE(hj->getIf<bool>("ok").value_or(false));
  EXPECT_TRUE(hj->getIf<bool>("host_running").value_or(false));

  pbr::StatusHttpRequest status{.method = "GET", .path = "/status", .authorization = {}};
  auto s = pbr::HandleStatusHttpRequest(status, {}, snap);
  EXPECT_EQ(s.status_code, 200);
  auto sj = pbr::TryParseObject(s.body);
  ASSERT_TRUE(sj);
  EXPECT_EQ(sj->getString("status").value_or(""), "reachable");
  EXPECT_EQ(sj->getString("listen").value_or(""), snap.listen_multiaddr);
  EXPECT_EQ(sj->getString("peer_id").value_or(""), snap.peer_id);
  EXPECT_TRUE(sj->getIf<bool>("circuit_relay").value_or(false));
  EXPECT_FALSE(sj->getIf<bool>("media_relay").value_or(true));
}

TEST(StatusHttpProtocolTest, BearerAuth) {
  pbr::StatusHttpAuthConfig auth;
  auth.bearer_token = "s3cret";
  pbr::StatusHttpSnapshot snap;
  snap.host_running = true;

  pbr::StatusHttpRequest missing{.method = "GET", .path = "/healthz", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(missing, auth, snap).status_code, 401);

  pbr::StatusHttpRequest bad{.method = "GET", .path = "/healthz", .authorization = "Bearer nope"};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(bad, auth, snap).status_code, 401);

  pbr::StatusHttpRequest ok{.method = "GET", .path = "/healthz", .authorization = "Bearer s3cret"};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(ok, auth, snap).status_code, 200);
}

TEST(StatusHttpProtocolTest, ParseRequestExtractsAuthorization) {
  const char* raw =
      "GET /status HTTP/1.1\r\n"
      "Host: 127.0.0.1:18518\r\n"
      "Authorization: Bearer tok\r\n"
      "\r\n";
  auto req = pbr::TryParseStatusHttpRequest(raw);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->method, "GET");
  EXPECT_EQ(req->path, "/status");
  EXPECT_EQ(req->authorization, "Bearer tok");
}

TEST(StatusHttpProtocolTest, UnknownPathAndMethod) {
  pbr::StatusHttpSnapshot snap;
  pbr::StatusHttpRequest missing{.method = "GET", .path = "/nope", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(missing, {}, snap).status_code, 404);

  pbr::StatusHttpRequest post{.method = "POST", .path = "/status", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(post, {}, snap).status_code, 405);
}
