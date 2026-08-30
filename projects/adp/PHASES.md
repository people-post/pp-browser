# Phases — ADP

## Phase 0 — Project + ADRs

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] ADRs A001–A007
- [x] Register in `projects/README.md` + `SRC_LAYOUT.md`

## Phase 1 — Codec + HMAC + DatagramIo + best-effort

- [x] `WireCodec`, `HmacBinder`, constants
- [x] `DatagramIo`, `MemoryDatagramIo`, `OsUdpDatagramIo`
- [x] `Endpoint` demux + `Connection` best-effort + path migrate + close
- [x] Tests: wire / HMAC / replay / skew / lifecycle / path / demux / NAT / best-effort

## Phase 2 — Reliable delivery

- [x] ACK + rtx + send window + virtual clock
- [x] Tests: reliable / ack / qos isolation / shutdown under loss

## Phase 3 — Harden

- [x] Packet mutilator, OsUdp loopback smoke, multi-connection stress
- [x] Promote `docs/contracts/ADP.md`
- [x] `pp_browser_adp_test` green

## Phase 4 — Opus media slice (product dogfood)

- [x] ADRs A008–A011
- [x] `CallMediaAdpPath` + HKDF + hello negotiate + bridge Opus path
- [x] TEMP dogfood gate `CallMediaAdpDogfood.h` (not settings/config)
- [ ] LAN dogfood; then delete gate → default-on

## Run tests

```bash
cmake -S . -B build -DPP_BROWSER_BUILD_TESTS=ON
cmake --build build --target pp_browser_adp_test pp_browser_p2p_test -j
./build/src/base/adp/tests/pp_browser_adp_test
./build/src/base/p2p/tests/pp_browser_p2p_test --gtest_filter='CallMediaAdp*'
```
