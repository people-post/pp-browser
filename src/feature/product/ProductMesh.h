#pragma once

#include "feature/conversations/HubMeshOwner.h"
#include "common/Module.h"

#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The app's mesh: one `MeshHost`, built for the profile's role and lent to the conversations hub
 * (`HubMeshOwner`). The node role's services — what it hosts and serves, the DHT and directory it
 * runs, its own directory row — come from domain/node, the same ones pp-node runs; messaging never
 * names them.
 */
class ProductMesh : public HubMeshOwner, public Module {
public:
  ProductMesh();
  ~ProductMesh() override;
  ProductMesh(const ProductMesh&) = delete;
  ProductMesh& operator=(const ProductMesh&) = delete;

  Roe<mesh::MeshHost*> Start(StartRequest request) override;
  void ConfigureQueries(const QueryInputs& inputs) override;
  void ApplyServing(const MeshConfig& config) override;
  void Stop() override;
  void Release() override;

private:
  void ConfigureDht(const QueryInputs& inputs, bool node_role);
  void ConfigureDirectory(const QueryInputs& inputs, bool node_role);

  std::unique_ptr<mesh::MeshHost> mesh_;
  /** A node tries UPnP on its first start only. */
  bool upnp_auto_tried_ = false;
};

} // namespace pbr
