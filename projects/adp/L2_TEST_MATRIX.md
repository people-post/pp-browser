# AMP L2 (MSH session) functional test matrix

**Tier:** A (pure L2; in-memory handshake + crypto unit tests)  
**Contract:** [docs/contracts/AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) · **Design:** [STACK.md](STACK.md)  
**Suite:** `pp_browser_amp_session_test` (`src/base/mesh/session/tests/`)

Cross-layer checks live in [TEST_MATRIX.md](TEST_MATRIX.md) (Tier B).

## Handshake

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-H-01 | MSH run establishes complementary sessions | `MshHandshakeTest.RunEstablishesComplementarySessions` | done |
| L2-H-02 | Tampered finished fails | `MshHandshakeTest.TamperedFinishedFails` | done |

## Session AEAD

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-C-01 | Seal/open round-trip | `SessionCryptoTest.SealOpenRoundTrip` | done |
| L2-C-02 | Wrong AAD fails open | `SessionCryptoTest.WrongAadFailsOpen` | done |
| L2-C-03 | Rekey rotates send/recv keys | `SessionTest.RekeyRotatesSendRecv` | done |

## Key derivation

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-K-01 | Transcript hash deterministic | `SessionKeysTest.TranscriptHashDeterministic` | done |
| L2-K-02 | Directional keys differ | `SessionKeysTest.DeriveDirectionalKeysDiffer` | done |
| L2-K-03 | Rekey changes send/recv material | `SessionKeysTest.RekeyChangesSendRecv` | done |

## Session control (ch0 wire)

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-S-01 | Control request/ack round-trip | `SessionControlCodecTest.RoundTripRequestAndAck` | done |
| L2-S-02 | Distinguishes from capability v1 | `SessionControlCodecTest.DistinguishesFromCapabilityVersionOne` | done |

## Rekey grace

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-R-01 | Previous epoch accepted within grace | `SessionRekeyGraceTest.ApplyRekeyAcceptsPreviousEpochWithinGrace` | done |
| L2-R-02 | Previous epoch rejected after grace | `SessionRekeyGraceTest.ApplyRekeyRejectsPreviousEpochAfterGrace` | done |
