#include "common/chat/MessagingJson.h"

#include <gtest/gtest.h>

TEST(RelayHistoryTest, HistoryRequestBuildsQueryString) {
  using namespace pbr;

  ChatHistoryRequest request;
  request.requester_identity_kind = "account";
  request.requester_identity_value = "account:local";
  request.peer_identity_kind = "account";
  request.peer_identity_value = "account:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.min_sender_seq = 10;
  request.max_sender_seq = 42;
  request.limit = 50;
  request.order = "asc";

  const std::string query = StreamHistoryRequestToQueryString(request);
  EXPECT_NE(query.find("sender_account_id=account:peer"), std::string::npos);
  EXPECT_NE(query.find("stream_id="), std::string::npos);
  EXPECT_NE(query.find("min_index_key=10"), std::string::npos);
  EXPECT_NE(query.find("max_index_key=42"), std::string::npos);
}
