#include "lib/net/OrgBackendClients.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

// N032: a mesh_node registers what it runs at the top level of register/finish; a person registers no such keys.
TEST(NodeClaimsTest, ARegistrationCarriesWhatTheNodeRunsOnlyWhenSet) {
  RegistrationPublishOpts publish;
  publish.entity_kind = "mesh_node";
  publish.amp_wire = 2;
  publish.node_version = "4.1.0";
  publish.features = {"hop_data_lane"};
  Object body;
  ApplyRegistrationPublishOpts(body, publish);
  EXPECT_EQ(body.getIf<int64_t>("amp_wire").value_or(0), 2);
  EXPECT_EQ(body.getString("node_version").value_or(""), "4.1.0");
  const Array* features = body.getArray("features");
  ASSERT_NE(features, nullptr);
  ASSERT_EQ(features->elements.size(), 1u);
  EXPECT_EQ(asString(features->elements[0]).value_or(""), "hop_data_lane");

  Object person;
  ApplyRegistrationPublishOpts(person, RegistrationPublishOpts{});
  EXPECT_FALSE(person.contains("amp_wire"));
  EXPECT_FALSE(person.contains("node_version"));
  EXPECT_FALSE(person.contains("features"));
}

// A row says what its node runs, or nothing (a node or directory before N032): both parse.
TEST(NodeClaimsTest, DirectoryRowsCarryWhatTheNodeSaysItRuns) {
  const std::string json = R"({"nodes": [
    {"relay_user_id": "relay:new", "amp_wire": 2, "node_version": "4.1.0", "features": ["hop_data_lane"],
     "endpoints": [{"peer_id": "QmNew", "multiaddrs": []}]},
    {"relay_user_id": "relay:old", "endpoints": [{"peer_id": "QmOld", "multiaddrs": []}]},
    {"relay_user_id": "relay:junk", "amp_wire": 99}
  ]})";
  auto hits = MeshNodeHitsFromDirectoryJson(json);
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 3u);
  EXPECT_EQ((*hits)[0].amp_wire, 2);
  EXPECT_EQ((*hits)[0].node_version, "4.1.0");
  EXPECT_EQ((*hits)[0].features, std::vector<std::string>{"hop_data_lane"});
  EXPECT_EQ((*hits)[1].amp_wire, 0) << "said nothing";
  EXPECT_TRUE((*hits)[1].node_version.empty());
  EXPECT_EQ((*hits)[2].amp_wire, 0) << "not a wire version (one nibble on the wire): taken as said nothing";
}

} // namespace
} // namespace pbr
