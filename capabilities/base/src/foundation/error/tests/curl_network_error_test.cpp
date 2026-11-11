#include "foundation/error/CurlNetworkError.h"

#include <curl/curl.h>
#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CurlNetworkErrorTest, NamesTheCause) {
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_COULDNT_RESOLVE_HOST), Err::Network::DnsFailed);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_COULDNT_CONNECT), Err::Network::ConnectFailed);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_OPERATION_TIMEDOUT), Err::Network::Timeout);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_SSL_CONNECT_ERROR), Err::Network::Offline);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_PEER_FAILED_VERIFICATION), Err::Network::TlsFailed);
}

TEST(CurlNetworkErrorTest, OnlyVerificationFailuresAreTls) {
  EXPECT_NE(NetworkCodeFromCurl(CURLE_SSL_CONNECT_ERROR), Err::Network::TlsFailed);
  for (CURLcode code : {CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CERTPROBLEM, CURLE_SSL_ISSUER_ERROR,
                        CURLE_SSL_PINNEDPUBKEYNOTMATCH, CURLE_SSL_INVALIDCERTSTATUS}) {
    EXPECT_EQ(NetworkCodeFromCurl(code), Err::Network::TlsFailed) << code;
  }
}

TEST(CurlNetworkErrorTest, OfflineAndTlsAreWordedApart) {
  EXPECT_NE(AppError::CatalogMessage(ErrorCategory::Network, static_cast<int32_t>(Err::Network::Offline)),
            AppError::CatalogMessage(ErrorCategory::Network, static_cast<int32_t>(Err::Network::TlsFailed)));
}

TEST(CurlNetworkErrorTest, UnknownCausesStayUnreachable) {
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_RECV_ERROR), Err::Network::Unreachable);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_GOT_NOTHING), Err::Network::Unreachable);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_WRITE_ERROR), Err::Network::Unreachable);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_COULDNT_RESOLVE_PROXY), Err::Network::Unreachable);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_SSL_CACERT_BADFILE), Err::Network::Unreachable);
  EXPECT_EQ(NetworkCodeFromCurl(CURLE_UNSUPPORTED_PROTOCOL), Err::Network::Unreachable);
}

} // namespace
} // namespace pbr
