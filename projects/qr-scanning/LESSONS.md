# Verification lessons

- QR contrast: SVG markup tests cannot catch global RCSS image tint. The rendered QR must be
  checked under both theme styles; the product QR class must preserve white rather than inherit
  icon tint. This belonged in visual/UI acceptance.
- Capture FPS: SDL's app-facing configured spec does not establish native delivery. Device
  acceptance must compare native frame duration, delivered dimensions and measured frame count.
  Continuous native FPS ranges need candidates within the range for SDL's discrete negotiation.
- Zoom: requested magnification is not current magnification during a native ramp. The regression
  uses a delayed fake camera, and pending ramps wait on native feedback. Centered zoom must retain
  an off-center candidate and quiet zone. These belonged in media policy tests and independent review.
- Reopen: destroying a native camera's SDL property group must also invalidate its ID. Camera
  control acceptance includes open→zoom→cancel→reopen; a single successful open is insufficient.
- Zoom race (Windows CI): the scanner read the zoom state after decoding, so a frame fetched
  mid-ramp was judged against the settled state and a second zoom compounded the first. The
  regression used a wall-clock ramp in the fake camera and only failed where decoding is slow.
  Media policy tests must make the fake's state transitions observable on query (settle when the
  scanner asks), never on elapsed time; a timing-based fake passes on fast machines and hides the race.
