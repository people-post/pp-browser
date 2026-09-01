# AMP integration test matrix (Track A)

**Tier:** B (local integration on `MemoryDatagramIo`)  
**Stack:** [STACK.md](STACK.md) · [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) · [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md)  
**Suite:** `pp_browser_amp_link_test` (`amp_integration_test.cpp`)

Rekey receive path uses a **grace window ≤ 1 s** for the previous epoch ([AMP-SESSION.md § Rekey](../../docs/contracts/AMP-SESSION.md)).

## Failure propagation matrix

| ID | STACK / contract row | gtest | Status |
|----|----------------------|-------|--------|
| A-INT-01 | Channel RESET — siblings continue | `AmpIntegrationTest.ChannelResetSiblingSurvives` | done |
| A-INT-02 | L3 Reliable → ADP Reliable under loss | `AmpIntegrationTest.ReliableDataSurvivesLoss` | done |
| A-INT-03 | One session per PeerId (dual-dial) | `AmpIntegrationTest.DualDialChannelsWorkAfterElection` | done |
| A-INT-04 | MSH fail → no Session | `AmpIntegrationTest.MshFailureNoConnectedMux` | done |
| A-INT-05 | Association Close → Session ends; recovery | `AmpIntegrationTest.AssociationCloseAndRecovery` | done |
| A-INT-06 | L1 path migrate — Session unchanged | `AmpIntegrationTest.PathMigrateMidSession` | done |
| A-INT-07 | L3 FRAG through L2 + L1 | `AmpIntegrationTest.LargeFragRoundTripThroughStack` | done |
| A-INT-08 | Wire rekey + epoch grace | `AmpIntegrationTest.WireRekeyWithGraceWindow` | done |
| A-INT-09 | Post-grace stale epoch dropped | `AmpIntegrationTest.PostGraceStaleEpochDropped` | done |
| A-INT-10 | MSH chunk loss + ADP rtx (stretch) | — | deferred |

## Session control wire (ch0)

| Field | Value |
|-------|-------|
| version | `2` (capability payloads use version `1`) |
| kind | `1` = RekeyRequest, `2` = RekeyAck |
| target_epoch | `u32` LE — must equal `local_epoch + 1` |
