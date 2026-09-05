# AMP L3 (channel mux) functional test matrix

**Tier:** A (in-memory MSH link via `amp_test_link.h`; no `PeerLinkManager`)  
**Contract:** [docs/contracts/AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) · **Design:** [STACK.md](STACK.md)  
**Suite:** `pp_browser_amp_channel_test` (`src/lib/amp/L3/tests/`)

Cross-layer FRAG / RESET checks live in [TEST_MATRIX.md](TEST_MATRIX.md) (Tier B).

## Wire codec

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L3-W-01 | OPEN round-trip | `ChannelWireTest.OpenRoundTrip` | done |
| L3-W-02 | DATA round-trip | `ChannelWireTest.DataRoundTrip` | done |

## Capability (ch0 payload)

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L3-C-01 | Capability encode/decode | `CapabilityTest.EncodeDecodeRoundTrip` | done |

## Message reassembly

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L3-F-01 | In-order FRAG assembly | `MessageReassemblyTest.AssemblesInOrder` | done |
| L3-F-02 | Duplicate frag dropped | `MessageReassemblyTest.DuplicateFragDropped` | done |
| L3-F-03 | Out-of-order assembly | `MessageReassemblyTest.AssemblesOutOfOrder` | done |
| L3-F-04 | Loss leaves partial incomplete | `MessageReassemblyTest.LossLeavesPartialIncomplete` | done |
| L3-F-05 | Sweep drops stale partial | `MessageReassemblyTest.SweepExpiredDropsStalePartial` | done |

## ChannelMux + ChannelSession

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L3-M-01 | Open + DATA round-trip | `ChannelMuxTest.OpenAndDataRoundTrip` | done |
| L3-M-02 | Realtime uses BestEffort QoS | `ChannelMuxTest.RealtimeUsesBestEffortQos` | done |
| L3-M-03 | RESET does not kill sibling | `ChannelMuxTest.ResetDoesNotKillSiblingChannel` | done |
| L3-M-04 | Large payload fragments | `ChannelMuxTest.LargePayloadFragments` | done |
| L3-M-05 | Capability channel 0 | `ChannelMuxTest.CapabilityChannelZero` | done |
| L3-M-06 | Inbound protocol handler | `ChannelMuxTest.InboundProtocolHandler` | done |
| L3-M-07 | Remote RESET notifies session | `ChannelMuxTest.RemoteResetNotifiesChannelSession` | done |
| L3-M-08 | ReadOnce closes after first frame | `ChannelSessionTest.ReadOnceClosesAfterFirstFrame` | done |
