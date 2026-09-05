# AMP link layer functional test matrix

**Tier:** A (link unit tests on `MemoryDatagramIo`; no Tier B adversarial matrix)  
**Contract:** [STACK.md](STACK.md) · **Design:** [DECISIONS.md](DECISIONS.md) (A025, A026)  
**Suite:** `pp_browser_amp_link_test` (`src/lib/amp/link/tests/`)

Tier B cross-layer integration: [TEST_MATRIX.md](TEST_MATRIX.md) → `pp_browser_amp_integration_test`.

## Multiaddr

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-A-01 | ADP multiaddr parse/format | `AdpMultiaddrTest.ParseAndFormatRoundTrip` | done |

## Association + channels

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-L-01 | EnsureAssociation over memory I/O | `MeshLinkTest.EnsureAssociationOverMemoryIo` | done |
| LINK-L-02 | OpenChannel DATA round-trip | `MeshLinkTest.OpenChannelDataRoundTrip` | done |
| LINK-L-03 | MeshRuntime pump drives assoc | `MeshRuntimeTest.PumpDrivesAssociationRoundTrip` | done |
| LINK-L-04 | Inbound link rekeys to registered alias | `MeshLinkTest.InboundLinkRekeysToRegisteredAlias` | done |

## Capability (ch0 over link)

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-C-01 | Capability exchange after assoc | `MeshLinkTest.CapabilityExchangeAfterAssociation` | done |
| LINK-C-02 | Ingest enables PeerId dial | `MeshLinkTest.CapabilityIngestEnablesPeerIdDial` | done |

## Dual-dial election

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-D-01 | One Connected link per PeerId | `MeshLinkTest.DualDialElectsOneConnectedLinkPerPeerId` | done |

## AmpStack

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-S-01 | Create + associate via stacks | `AmpStackTest.CreateAndAssociateViaStacks` | done |
