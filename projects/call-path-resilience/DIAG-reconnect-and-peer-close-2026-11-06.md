# Diagnosis: reconnect window never redials; peer_close shown as a failure

**Date:** 2026-11-06 · **Source:** iPhone logs from tonight's device tests · **For:** Hongwei (call connection)

**Outcome (2026-11-07):** folded into [K014](DECISIONS.md#k014--a-hangup-says-bye-a-close-without-it-is-a-lost-path-a-connected-call-drops-it-never-times-out) — a hangup now says call-media `bye` before the close, a close without it is a lost path (reconnect), a connected call that loses its media is "Call dropped" (never `ConnectFailed` / "Mesh connect timeout"), and the re-anchor reaches with the first connect's policy and advertised relays. Suggestion 1 (redial on relay recovery) is not built: the re-anchor already retries every 2 s in the window; see K014. Moved here from `projects/p2p-av-calls/` (branch `kenneth/diag-reconnect-peer-close`).

## Summary

1. A call that loses its path during a signal hole does not redial when the relay links come back; it waits out the 30 s window and closes itself.
2. `peer_close` on the call-media stream is reported as `ConnectFailed` ("通话已中断"), the same signature as a normal remote hangup; the user cannot tell "other side hung up" from "link died".
3. Both are in the call-connection layer (`CallMediaLeg` / `CallDirectPath`), so we are reporting, not patching.
4. Context only: all these calls ran on `path=circuit` because `direct upgrade: not for this pair (a mobile end anchors on the relay)`.

Logs: `logs/iphone-calls-1106-2052/` (`pp-browser.log`, `pp-browser.1.log`).

## Finding 1: reconnect window never redials (call b27bdae9)

`pp-browser.1.log` (iPhone), no `network.change` anywhere in the window (a signal hole, not an interface change):

```
20:06:49.257 [MeshLink] link dropped ... remote=172.126.108.181:4001 reason=connection-dead was_connected=1  (last_rx_age_ms=8712)
20:06:50.284 [CallMediaLeg] CallMediaLeg path lost call_id=call:b27bdae9-... reason=active path silent, no standby — reconnecting (window 30000ms)
20:06:51.515 [MeshLink] link dropped ... remote=35.158.70.252:443 reason=connection-dead  (last_rx_age_ms=5002)
20:06:52.285 [MeshLink] link dropped ... remote=34.233.23.235:443 reason=connection-dead  (last_rx_age_ms=5004)
20:06:58.019 [MeshLink] link connected ... remote=35.158.70.252:443
20:06:58.020 [CircuitRendezvous] circuit rendezvous re-park on reconnect peer=Qmc1TDTb6K...
20:07:05.737 [MeshLink] link connected ... remote=172.126.108.181:4001
20:07:05.737 [CircuitRendezvous] circuit rendezvous re-park on reconnect peer=QmdMR5iWDS...
20:07:20.286 [CallMediaLeg] CallMediaLeg reconnect window expired call_id=call:b27bdae9-...
20:07:20.302 [CallDirectPath] Call-media failed ... reason=amp call-media: no path within the reconnect window
20:07:20.334 [CallController] in-call subtitle="通话已中断" phase=ConnectFailed status=Failed
```

Between 20:06:58 and 20:07:20 the log has no media connect or redial toward the peer; the only dials are to relays. The relay links were back 22 s and 14 s before the window expired, and the circuit was re-parked, yet the call still failed with "no path within the reconnect window".

## Finding 2: peer_close reported as a connection failure (call 62914ac7)

`pp-browser.1.log` (iPhone):

```
20:29:55.375 [Runtime.Lifecycle] Entering background
20:29:55.985 [CallMediaLeg] CallMediaLeg teardown call_id=call:62914ac7-... phase=MediaReady ... reason=call-media stream closed (peer_close)
20:29:56.519 [CallDirectPath] Mesh connect timeout call_id=call:62914ac7-...
20:29:56.541 [LiveCall] status=DirectLive->Failed phase=InCall->ConnectFailed reason=ConnectFailed
20:29:56.542 [CallController] call media error shown as call.error.connect_failed raw=mesh connect timeout
20:29:56.552 [CallController] in-call subtitle="通话已中断" phase=ConnectFailed status=Failed
20:29:57.925 [Diag] event=network.change gen=4 ... transport=2 expensive=1 attachment_changed=1
```

Same signature on a normal remote hangup (call afeb6094, `pp-browser.log`):

```
20:37:11.351 [CallMediaLeg] CallMediaLeg teardown call_id=call:afeb6094-... reason=call-media stream closed (peer_close)
20:37:11.601 [CallDirectPath] Mesh connect timeout call_id=call:afeb6094-...
20:37:11.610 [LiveCall] status=DirectLive->Failed phase=InCall->ConnectFailed reason=ConnectFailed
20:37:11.623 [CallController] in-call subtitle="通话已中断" ...
20:37:12.270 [LiveCall] close call_id=call:afeb6094-... instance=1 was=Joined reason=RemoteEnded
```

So `peer_close` leads to "Mesh connect timeout" -> `ConnectFailed` in both cases; the real end reason (`RemoteEnded`) arrives about 0.7 s later and is not used for the on-screen text.

Caveat for 62914ac7: the phone entered background 0.6 s before the close and the Wi-Fi to cellular change was logged 2 s after it. From one side we cannot tell who closed first.

## Suggested changes

1. **Redial on relay recovery.** When a relay link to the peer's circuit comes back inside the reconnect window (the `re-park on reconnect` event), immediately re-run the media connect (or re-park on the circuit) instead of waiting for the window to expire.
2. **peer_close handling.** On `peer_close`, wait briefly for the control-channel bye before declaring failure. If no bye arrives, map "peer_close without bye" to its own end reason (peer link lost) so the UI can say "对方掉线了" instead of "通话已中断". A `peer_close` followed by a bye stays a normal remote hangup.

## What we are not asking for

- No change to the 30 s window length or to the direct-upgrade rule for mobile ends.
- No change to Amp/mesh link keepalive timing (`connection-dead` after 5-9 s of silence looks reasonable for a signal hole).
- No client-side workaround from us; we will only change the UI text once a distinct end reason exists.
