#pragma once

#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Rml {
class Element;
} // namespace Rml

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
    /** Premultiplied RGBA8 (matches CallMediaEngine::VideoTileFrame). */
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

  /** Upload if needed and letterbox-draw into `element`. UI thread, GL context current. */
  void RenderTile(CallVideoTileKind kind, Rml::Element* element, uint32_t stream_id = 0);

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
  void DrawTile(Rml::Element* element, GpuTile& tile);
  void ReleaseTile(GpuTile& tile);

  GpuTile remote_;
  GpuTile local_;
  std::unordered_map<uint32_t, GpuTile> peers_;
};

} // namespace pbr
