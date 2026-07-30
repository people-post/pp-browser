#pragma once

#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <vector>

class RenderInterface_GL3;

namespace Rml {
class Context;
class Element;
} // namespace Rml

namespace pbr {

/** V018: persistent GL textures blitted into shell call tile elements. */
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

  /** Upload pending pixels and draw into `#call-remote-tile` / `#call-local-tile`. UI thread only. */
  void Draw(Rml::Context* context, RenderInterface_GL3& renderer);

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
  void DrawTile(Rml::Element* element, GpuTile& tile, RenderInterface_GL3& renderer);

  GpuTile remote_;
  GpuTile local_;
};

} // namespace pbr
