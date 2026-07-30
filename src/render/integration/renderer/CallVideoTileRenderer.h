#pragma once

#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <vector>

namespace Rml {
class Element;
} // namespace Rml

namespace pbr {

enum class CallVideoTileKind { Remote, Local };

/**
 * V018: persistent GL textures for call tiles. Ownership stays here; paint happens
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
  void Clear();

  /** Upload if needed and letterbox-draw into `element`. UI thread, GL context current. */
  void RenderTile(CallVideoTileKind kind, Rml::Element* element);

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

  GpuTile remote_;
  GpuTile local_;
};

} // namespace pbr
