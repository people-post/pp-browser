#include "app/node/StatusHttpProtocol.h"

#include "common/ValueJson.h"
#include "common/metrics/MetricsRegistry.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include "common/PbrCompat.h"

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
  snap.listen_multiaddr = "/ip6/2001:db8::1/udp/443/adp/1.0.0/p2p/12D3KooWTest";
  snap.listen_multiaddrs = {snap.listen_multiaddr,
                            "/ip4/10.0.0.5/udp/443/adp/1.0.0/p2p/12D3KooWTest"};
  snap.peer_id = "12D3KooWtest";
  snap.circuit_relay = true;
  snap.media_relay = false;
  snap.dht = true;
  snap.dht_json = R"({"started":true,"cached_records":2})";
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
  const auto* listen_addrs = sj->getArray("listen_addrs");
  ASSERT_NE(listen_addrs, nullptr);
  ASSERT_EQ(listen_addrs->elements.size(), 2u);
  EXPECT_EQ(pbr::asString(listen_addrs->elements[0]).value_or(""), snap.listen_multiaddrs[0]);
  EXPECT_EQ(sj->getString("peer_id").value_or(""), snap.peer_id);
  EXPECT_TRUE(sj->getIf<bool>("circuit_relay").value_or(false));
  EXPECT_FALSE(sj->getIf<bool>("media_relay").value_or(true));
  EXPECT_TRUE(sj->getIf<bool>("dht").value_or(false));
  ASSERT_NE(sj->getObject("dht_stats"), nullptr);
  EXPECT_EQ(sj->getObject("dht_stats")->getIf<int64_t>("cached_records").value_or(0), 2);
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

// node-monitoring M1: operators scrape Prometheus text, behind the same bearer auth.
TEST(StatusHttpProtocolTest, MetricsServesPrometheusTextBehindAuth) {
  pbr::MetricsRegistry::Global().Counter("pp_test_scrape_total", "Test counter.").Inc();
  pbr::StatusHttpAuthConfig auth;
  auth.bearer_token = "s3cret";
  pbr::StatusHttpSnapshot snap;

  pbr::StatusHttpRequest missing{.method = "GET", .path = "/metrics", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(missing, auth, snap).status_code, 401);

  pbr::StatusHttpRequest ok{.method = "GET", .path = "/metrics", .authorization = "Bearer s3cret"};
  const auto response = pbr::HandleStatusHttpRequest(ok, auth, snap);
  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(response.content_type.rfind("text/plain; version=0.0.4", 0), 0u);
  EXPECT_NE(response.body.find("# TYPE pp_test_scrape_total counter"), std::string::npos);
}

// A collector that throws fails that scrape with a 500; it must not unwind the server (and kill pp-node).
TEST(StatusHttpProtocolTest, MetricsCollectorThrowingAnswers500) {
  pbr::StatusHttpAuthConfig auth;
  pbr::StatusHttpSnapshot snap;
  pbr::StatusHttpRequest req{.method = "GET", .path = "/metrics", .authorization = {}};
  {
    pbr::ScopedMetricsCollector bad(pbr::MetricsRegistry::Global(),
                                    [](pbr::MetricsRegistry&) { throw std::out_of_range("collector"); });
    EXPECT_EQ(pbr::HandleStatusHttpRequest(req, auth, snap).status_code, 500);
  }
  EXPECT_EQ(pbr::HandleStatusHttpRequest(req, auth, snap).status_code, 200) << "the next scrape works";
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

// Lab only: off unless pp-node sets the action; a PeerId must be base58.
TEST(StatusHttpProtocolTest, DhtFindIsOffUnlessSetAndTakesOnlyAPeerId) {
  pbr::StatusHttpSnapshot snap;
  const auto parsed = pbr::TryParseStatusHttpRequest(
      "GET /dht/find?peer_id=QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(parsed.has_value());
  const pbr::StatusHttpRequest find = *parsed;
  EXPECT_EQ(pbr::HandleStatusHttpRequest(find, {}, snap).status_code, 404);

  std::vector<std::string> asked;
  pbr::StatusHttpActions actions;
  actions.dht_find = [&](const std::string& peer_id) {
    asked.push_back(peer_id);
    return true;
  };
  auto started = pbr::HandleStatusHttpRequest(find, {}, snap, actions);
  EXPECT_EQ(started.status_code, 202);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked.front(), "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");

  pbr::StatusHttpRequest bad{.method = "GET", .path = "/dht/find", .query = "peer_id=Qm0l&x=1", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(bad, {}, snap, actions).status_code, 400);
  EXPECT_EQ(asked.size(), 1u) << "not asked";

  pbr::StatusHttpAuthConfig auth;
  auth.bearer_token = "secret";
  EXPECT_EQ(pbr::HandleStatusHttpRequest(find, auth, snap, actions).status_code, 401) << "behind the token too";
}

TEST(StatusHttpProtocolTest, UnknownPathAndMethod) {
  pbr::StatusHttpSnapshot snap;
  pbr::StatusHttpRequest missing{.method = "GET", .path = "/nope", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(missing, {}, snap).status_code, 404);

  pbr::StatusHttpRequest post{.method = "POST", .path = "/status", .authorization = {}};
  EXPECT_EQ(pbr::HandleStatusHttpRequest(post, {}, snap).status_code, 405);
}

TEST(StatusHttpProtocolTest, ParseRequestWaitsForAnnouncedBody) {
  const std::string body = R"({"state":"retiring"})";
  ASSERT_EQ(body.size(), 20u);
  const std::string head20 =
      "POST /control/state HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 20\r\n\r\n";
  EXPECT_FALSE(pbr::TryParseStatusHttpRequest(head20 + body.substr(0, 5)));
  auto req = pbr::TryParseStatusHttpRequest(head20 + body);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->method, "POST");
  EXPECT_EQ(req->body, body);
  EXPECT_FALSE(pbr::TryParseStatusHttpRequest("POST /x HTTP/1.1\r\nContent-Length: 99999\r\n\r\n"));
}

namespace {

pbr::StatusHttpResponse PostState(const std::string& body, const pbr::StatusHttpActions& actions,
                                  const pbr::StatusHttpAuthConfig& auth = {},
                                  const std::string& authorization = {}) {
  pbr::StatusHttpRequest req;
  req.method = "POST";
  req.path = "/control/state";
  req.authorization = authorization;
  req.content_type = "application/json; charset=utf-8";
  req.host = "127.0.0.1:18518";
  req.body = body;
  return pbr::HandleStatusHttpRequest(req, auth, {}, actions);
}

} // namespace

TEST(StatusHttpProtocolTest, ControlStateAppliesValidStates) {
  std::string applied;
  pbr::StatusHttpActions actions;
  actions.set_state = [&](const std::string& s) {
    applied = s;
    return true;
  };
  const auto r = PostState(R"({"state":"draining"})", actions);
  EXPECT_EQ(r.status_code, 200);
  EXPECT_EQ(applied, "draining");
}

TEST(StatusHttpProtocolTest, ControlStateRejectsBadInput) {
  bool called = false;
  pbr::StatusHttpActions actions;
  actions.set_state = [&](const std::string&) {
    called = true;
    return true;
  };
  EXPECT_EQ(PostState(R"({"state":"sleeping"})", actions).status_code, 400);
  EXPECT_EQ(PostState(R"({})", actions).status_code, 400);
  EXPECT_EQ(PostState("not json", actions).status_code, 400);
  EXPECT_FALSE(called);
}

TEST(StatusHttpProtocolTest, ControlStateRefusedMoveIs409) {
  pbr::StatusHttpActions actions;
  actions.set_state = [](const std::string&) { return false; };
  EXPECT_EQ(PostState(R"({"state":"active"})", actions).status_code, 409);
}

// No callback is what a non-loopback bind gets: the route is closed.
TEST(StatusHttpProtocolTest, ControlStateClosedWithoutCallback) {
  EXPECT_EQ(PostState(R"({"state":"draining"})", {}).status_code, 405);
}

TEST(StatusHttpProtocolTest, ControlStateHonoursBearerToken) {
  int calls = 0;
  pbr::StatusHttpActions actions;
  actions.set_state = [&](const std::string&) {
    ++calls;
    return true;
  };
  pbr::StatusHttpAuthConfig auth;
  auth.bearer_token = "s3cret";
  EXPECT_EQ(PostState(R"({"state":"draining"})", actions, auth).status_code, 401);
  EXPECT_EQ(PostState(R"({"state":"draining"})", actions, auth, "Bearer s3cret").status_code, 200);
  EXPECT_EQ(calls, 1);
}

TEST(StatusHttpProtocolTest, PostElsewhereStaysMethodNotAllowed) {
  pbr::StatusHttpActions actions;
  actions.set_state = [](const std::string&) { return true; };
  pbr::StatusHttpRequest req;
  req.method = "POST";
  req.path = "/status";
  EXPECT_EQ(pbr::HandleStatusHttpRequest(req, {}, {}, actions).status_code, 405);
}

TEST(StatusHttpProtocolTest, ControlStateRefusesBrowserAndRebindingRequests) {
  int calls = 0;
  pbr::StatusHttpActions actions;
  actions.set_state = [&](const std::string&) {
    ++calls;
    return true;
  };
  auto post = [&](const std::string& content_type, const std::string& host, bool origin) {
    pbr::StatusHttpRequest req;
    req.method = "POST";
    req.path = "/control/state";
    req.content_type = content_type;
    req.host = host;
    req.has_origin = origin;
    req.body = R"({"state":"retiring"})";
    return pbr::HandleStatusHttpRequest(req, {}, {}, actions).status_code;
  };
  EXPECT_EQ(post("text/plain", "127.0.0.1:18518", false), 415);
  EXPECT_EQ(post("", "127.0.0.1:18518", false), 415);
  EXPECT_EQ(post("application/json", "127.0.0.1:18518", true), 403);
  EXPECT_EQ(post("application/json", "evil.example:18518", false), 403);
  EXPECT_EQ(post("application/json", "127.0.0.1.evil.example", false), 403);
  EXPECT_EQ(post("application/json", "", false), 403);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(post("application/json", "localhost:1", false), 200);
  EXPECT_EQ(post("application/json", "[::1]:18518", false), 200);
  EXPECT_EQ(post("Application/JSON", "127.0.0.1", false), 200);
  EXPECT_EQ(calls, 3);
}

TEST(StatusHttpProtocolTest, ParseRequestExtractsContentTypeHostOrigin) {
  auto req = pbr::TryParseStatusHttpRequest(
      "POST /control/state HTTP/1.1\r\nHost: 127.0.0.1:1\r\nOrigin: http://x\r\n"
      "Content-Type: application/json\r\nContent-Length: 0\r\n\r\n");
  ASSERT_TRUE(req);
  EXPECT_EQ(req->host, "127.0.0.1:1");
  EXPECT_EQ(req->content_type, "application/json");
  EXPECT_TRUE(req->has_origin);
}
