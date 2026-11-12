#pragma once

#include <ui/base/Types.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ui {
class Element;
} // namespace ui

namespace pbr {

enum class CallVideoTileKind { Remote, Local, Peer };

/**
 * V018/V034: persistent GL textures for call tiles. Ownership stays here; paint happens
 * from ElementCallVideoTile::OnRender (in-document stacking, no post-Context blit).
 */
class CallVideoTileRenderer {
public:
  struct Frame {
    int width = 0;
    int height = 0;
    uint64_t seq = 0;
    /** Premultiplied RGBA8 (matches MediaEngine::VideoTileFrame). */
    std::vector<uint8_t> rgba;
  };

  static CallVideoTileRenderer& Instance();

  void SubmitRemoteFrame(Frame frame);
  void SubmitLocalFrame(Frame frame);
  void SubmitPeerFrame(uint32_t stream_id, Frame frame);
  void Clear();
  /** Drop remote tile pixels (peer leave / camera off / stall) without touching local PiP. */
  void ClearRemote();
  void ClearPeer(uint32_t stream_id);
  /** Drop peer GPU tiles whose stream_id is not in `keep`. */
  void RetainPeers(const std::vector<uint32_t>& keep);
  /**
   * The seq of the frame a tile holds (0: none — never submitted, or let go by a Clear*, RetainPeers or a GPU reset).
   * The UI copies a frame out of the engine only when it differs from this one.
   */
  uint64_t HeldSeq(CallVideoTileKind kind, uint32_t stream_id = 0) const;

  /** Upload if needed and letterbox-draw into `element`. UI thread, GL context current. */
  /**
   * fill: cover the tile, cropping the frame's overflow (the spotlight, V059); else fit the whole frame in,
   * letterboxed.
   */
  void RenderTile(CallVideoTileKind kind, ui::Element* element, uint32_t stream_id = 0, bool fill = false);

  void ReleaseGpuResources();

private:
  CallVideoTileRenderer() = default;

  struct GpuTile {
    Frame pending;
    uint64_t uploaded_seq = 0;
    unsigned gl_tex = 0;
    int tex_width = 0;
    int tex_height = 0;
  };

  void UploadIfNeeded(GpuTile& tile);
  void DrawTile(ui::Element* element, GpuTile& tile, bool fill);
  void ReleaseTile(GpuTile& tile);

  GpuTile remote_;
  GpuTile local_;
  std::unordered_map<uint32_t, GpuTile> peers_;
};

} // namespace pbr
