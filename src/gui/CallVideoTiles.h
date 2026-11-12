#pragma once

#include "CallVideoTileRenderer.h"
#include "gui/UiFrameStats.h"
#include "lib/media/IMediaView.h"
#include "lib/ui/ShellTypes.h"

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace pbr {

/** A roster row's publisher stream id (0: none). */
inline uint32_t RowStream(const CallRosterParticipantState& row) {
  return static_cast<uint32_t>(std::strtoul(row.stream_id.c_str(), nullptr, 10));
}

/** An engine frame as the tiles take it. */
inline CallVideoTileRenderer::Frame TileFrame(IMediaView::VideoTileFrame&& tile) {
  CallVideoTileRenderer::Frame frame;
  frame.width = tile.width;
  frame.height = tile.height;
  frame.seq = tile.seq;
  frame.rgba = std::move(tile.rgba);
  return frame;
}

/** What this frame's self-view / 1:1 peer reads found. */
struct CallVideoTilesShown {
  bool local_preview = false; // there is a self-view picture (new or the one the tile holds)
  bool self_new = false;
  bool peer_new = false;
};

/**
 * This frame's self-view and 1:1 peer pictures (DEV/PLAN-call-frame-cost.md step 2). Each is copied out of the engine
 * only when it is not the frame its tile already holds (CallVideoTileRenderer::HeldSeq, which a Clear / ClearRemote /
 * GPU reset sets back to 0): no picture → no self-view; the same picture → nothing copied or submitted, the self-view
 * stays; a new picture → copied and submitted. `Media` is IMediaView (a template so tests can pass a small fake).
 */
template <class Media>
CallVideoTilesShown SubmitCallVideoTiles(Media& media, CallVideoTileRenderer& tiles, const bool remote_video) {
  CallVideoTilesShown shown;
  IMediaView::VideoTileFrame local;
  switch (media.CopyLocalVideoFrameIfNew(tiles.HeldSeq(CallVideoTileKind::Local), local)) {
    case IMediaView::FrameRead::None:
      break;
    case IMediaView::FrameRead::Same:
      shown.local_preview = true;
      break;
    case IMediaView::FrameRead::New:
      shown.local_preview = true;
      shown.self_new = true;
      UiCostCounters::Instance().AddFrameCopy(false);
      tiles.SubmitLocalFrame(TileFrame(std::move(local)));
      break;
  }
  IMediaView::VideoTileFrame remote;
  if (remote_video && media.CopyRemoteVideoFrameIfNew(tiles.HeldSeq(CallVideoTileKind::Remote), remote) ==
                          IMediaView::FrameRead::New) {
    shown.peer_new = true;
    UiCostCounters::Instance().AddFrameCopy(false);
    tiles.SubmitRemoteFrame(TileFrame(std::move(remote)));
  }
  return shown;
}

/**
 * The roster's peer pictures: each live stream's frame is copied only when it is not the one its tile holds; the
 * tiles of streams no longer live are let go (RetainPeers), so a stream id taken by someone new starts from nothing.
 * Sets each row's has_remote_video. Returns the live streams.
 */
template <class Media>
std::vector<uint32_t> SubmitCallRosterTiles(Media& media, CallVideoTileRenderer& tiles,
                                            std::vector<CallRosterParticipantState>& roster) {
  std::vector<uint32_t> live_peer_streams;
  for (CallRosterParticipantState& row : roster) {
    if (row.is_local) {
      row.has_remote_video = false;
      continue;
    }
    const uint32_t stream = RowStream(row);
    row.has_remote_video = stream != 0 && media.HasRemoteVideoForStream(stream);
    if (!row.has_remote_video) {
      continue;
    }
    live_peer_streams.push_back(stream);
    IMediaView::VideoTileFrame peer_tile;
    if (media.CopyRemoteVideoFrameForStreamIfNew(stream, tiles.HeldSeq(CallVideoTileKind::Peer, stream), peer_tile) ==
        IMediaView::FrameRead::New) {
      UiCostCounters::Instance().AddFrameCopy(false);
      tiles.SubmitPeerFrame(stream, TileFrame(std::move(peer_tile)));
    }
  }
  tiles.RetainPeers(live_peer_streams);
  return live_peer_streams;
}

} // namespace pbr
