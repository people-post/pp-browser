#include "common/PlatformLimits.h"
#include "common/tests/LocalHttpServer.h"
#include "domain/net/HttpClient.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

/** Sets an env var for the lifetime of the object and restores the prior value on destruction. */
class ScopedEnv {
public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* prev = std::getenv(name)) {
      previous_ = prev;
    }
    Apply(value);
  }

  ~ScopedEnv() {
    if (previous_) {
      Apply(previous_->c_str());
    } else {
      Apply(nullptr);
    }
  }

private:
  void Apply(const char* value) {
#if defined(_WIN32)
    _putenv_s(name_, value ? value : "");
#else
    if (value) {
      setenv(name_, value, /*overwrite=*/1);
    } else {
      unsetenv(name_);
    }
#endif
  }

  const char* name_;
  std::optional<std::string> previous_;
};

class HttpClientTest : public ::testing::Test {
protected:
  std::string WriteResponse(const size_t bytes) {
    server_.SetResponse(std::string(bytes, '\0'));
    return server_.Url();
  }

  pbr::test::LocalHttpServer server_;
};

TEST_F(HttpClientTest, UsesDefaultResponseLimit) {
  EXPECT_EQ(pbr::kMaxHttpClientBodyBytes, 8U * 1024U * 1024U);

  auto response = pbr::HttpClient::Get(WriteResponse(pbr::kMaxHttpClientBodyBytes));

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), pbr::kMaxHttpClientBodyBytes);

  const auto over_limit = pbr::HttpClient::Get(WriteResponse(pbr::kMaxHttpClientBodyBytes + 1));
  ASSERT_FALSE(over_limit);
  EXPECT_NE(over_limit.error().message.find("configured limit of 8388608 bytes"), std::string::npos);
}

TEST_F(HttpClientTest, HonorsExplicitResponseLimit) {
  auto response = pbr::HttpClient::Get(WriteResponse(6), {}, 6);

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), 6U);
}

TEST_F(HttpClientTest, AbortsReadWhenResponseExceedsLimit) {
  const auto response = pbr::HttpClient::Get(WriteResponse(6), {}, 5);

  ASSERT_FALSE(response);
  EXPECT_NE(response.error().message.find("configured limit of 5 bytes"), std::string::npos);
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsPlainHttpLoopback) {
  // Attachment/profile-icon URLs are peer-controlled; restrict_to_public_https must refuse
  // both the non-https scheme and the loopback destination the local test server binds to.
  const auto response =
      pbr::HttpClient::Get(WriteResponse(1), {}, pbr::kMaxHttpClientBodyBytes, pbr::HttpTimeout{},
                           /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsHttpsLoopbackByAddressNotProtocol) {
  // https:// is an allowed scheme, so a failure here must come from OpenPublicOnlySocket
  // rejecting the resolved loopback address, not from CURLOPT_PROTOCOLS_STR.
  const auto response = pbr::HttpClient::Get("https://127.0.0.1:1/", {}, pbr::kMaxHttpClientBodyBytes,
                                             pbr::HttpTimeout{}, /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().message.find("Unsupported protocol"), std::string::npos) << response.error().message;
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsHttpsIpv6LoopbackByAddressNotProtocol) {
  const auto response = pbr::HttpClient::Get("https://[::1]:1/", {}, pbr::kMaxHttpClientBodyBytes, pbr::HttpTimeout{},
                                             /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().message.find("Unsupported protocol"), std::string::npos) << response.error().message;
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsIpv6UniqueLocal) {
  // fc00::/7 (ULA) is a private range distinct from loopback/link-local; must be rejected too.
  const auto response = pbr::HttpClient::Get("https://[fd00::1]:1/", {}, pbr::kMaxHttpClientBodyBytes,
                                             pbr::HttpTimeout{}, /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().message.find("Unsupported protocol"), std::string::npos) << response.error().message;
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsIpv6LinkLocal) {
  const auto response = pbr::HttpClient::Get("https://[fe80::1]:1/", {}, pbr::kMaxHttpClientBodyBytes,
                                             pbr::HttpTimeout{}, /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().message.find("Unsupported protocol"), std::string::npos) << response.error().message;
}

TEST_F(HttpClientTest, RestrictToPublicHttpsRejectsIpv4MappedLoopback) {
  // ::ffff:127.0.0.1 must be classified by its embedded IPv4 address, not treated as public
  // just because it is wrapped in an IPv6 literal.
  const auto response = pbr::HttpClient::Get("https://[::ffff:127.0.0.1]:1/", {}, pbr::kMaxHttpClientBodyBytes,
                                             pbr::HttpTimeout{}, /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().message.find("Unsupported protocol"), std::string::npos) << response.error().message;
}

TEST_F(HttpClientTest, RestrictToPublicHttpsIgnoresEnvironmentProxy) {
  // Even if a proxy is configured, OpenPublicOnlySocket must see (and reject) the target's own
  // address rather than the proxy's, so a proxy cannot be used to tunnel to an internal host.
  ScopedEnv env("https_proxy", "http://127.0.0.1:1/");
  const auto response = pbr::HttpClient::Get("https://[fd00::1]:1/", {}, pbr::kMaxHttpClientBodyBytes,
                                             pbr::HttpTimeout{}, /*restrict_to_public_https=*/true);
  ASSERT_FALSE(response);
}

TEST_F(HttpClientTest, DefaultCallsStillAllowPlainHttpLoopback) {
  // Sanity: the opt-in guard must not change behavior for callers that don't ask for it
  // (e.g. org backend / relay clients that may legitimately use local dev endpoints).
  const auto response = pbr::HttpClient::Get(WriteResponse(3));
  ASSERT_TRUE(response) << response.error().message;
}

} // namespace
