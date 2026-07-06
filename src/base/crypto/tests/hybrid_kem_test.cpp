#include "base/crypto/AutoKeyEstablishment.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(HybridKemTest, RoundTripEncapsulationMatchesMasterPskDerivation) {
  auto alice = HybridKem::GenerateKeyPair();
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(alice));
  ASSERT_TRUE(static_cast<bool>(bob));

  std::string key_init_b64;
  auto shared = HybridKem::Encapsulate(bob->public_key, key_init_b64);
  ASSERT_TRUE(static_cast<bool>(shared));
  EXPECT_EQ(shared->size(), kHybridKemSharedSecretBytes);
  EXPECT_FALSE(key_init_b64.empty());

  auto decapsulated = HybridKem::Decapsulate(bob->private_key, key_init_b64);
  ASSERT_TRUE(static_cast<bool>(decapsulated));
  EXPECT_EQ(*shared, *decapsulated);

  auto master_from_encap = AutoKeyEstablishment::DeriveMasterPskFromSharedSecret(*shared);
  auto master_from_decap = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(bob->private_key, key_init_b64);
  ASSERT_TRUE(static_cast<bool>(master_from_encap));
  ASSERT_TRUE(static_cast<bool>(master_from_decap));
  EXPECT_EQ(*master_from_encap, *master_from_decap);
  EXPECT_EQ(master_from_encap->size(), kMasterPskSize);
}

} // namespace
} // namespace pbr
