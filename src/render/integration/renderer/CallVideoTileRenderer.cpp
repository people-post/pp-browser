#include "CallVideoTileRenderer.h"

#include "RmlUi_Backend.h"
#include "RmlUi_Renderer_GL3.h"

#if defined(RMLUI_GL_ES3)
	#if defined(__APPLE__) && TARGET_OS_IPHONE
		#include <OpenGLES/ES3/gl.h>
	#else
		#include <GLES3/gl3.h>
	#endif
#else
	#include "RmlUi_Include_GL3.h"
#endif

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

namespace pbr {

CallVideoTileRenderer& CallVideoTileRenderer::Instance() {
  static CallVideoTileRenderer instance;
  return instance;
}

void CallVideoTileRenderer::SubmitRemoteFrame(Frame frame) {
  remote_.pending = std::move(frame);
}

void CallVideoTileRenderer::SubmitLocalFrame(Frame frame) {
  local_.pending = std::move(frame);
}

void CallVideoTileRenderer::Clear() {
  ReleaseGpuResources();
  remote_.pending = {};
  local_.pending = {};
}

void CallVideoTileRenderer::ClearRemote() {
  if (remote_.gl_tex) {
    glDeleteTextures(1, &remote_.gl_tex);
  }
  remote_.gl_tex = 0;
  remote_.tex_width = 0;
  remote_.tex_height = 0;
  remote_.uploaded_seq = 0;
  remote_.pending = {};
}

void CallVideoTileRenderer::ReleaseGpuResources() {
  if (remote_.gl_tex) {
    glDeleteTextures(1, &remote_.gl_tex);
  }
  if (local_.gl_tex) {
    glDeleteTextures(1, &local_.gl_tex);
  }
  remote_.gl_tex = 0;
  local_.gl_tex = 0;
  remote_.tex_width = 0;
  remote_.tex_height = 0;
  local_.tex_width = 0;
  local_.tex_height = 0;
  remote_.uploaded_seq = 0;
  local_.uploaded_seq = 0;
}

void CallVideoTileRenderer::UploadIfNeeded(GpuTile& tile) {
  const Frame& frame = tile.pending;
  if (frame.seq == 0 || frame.width <= 0 || frame.height <= 0 ||
      frame.rgba.size() < static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4) {
    return;
  }
  if (tile.uploaded_seq == frame.seq) {
    return;
  }

  if (tile.gl_tex == 0) {
    glGenTextures(1, &tile.gl_tex);
    glBindTexture(GL_TEXTURE_2D, tile.gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, tile.gl_tex);
  }

  const int w = frame.width;
  const int h = frame.height;
  if (w != tile.tex_width || h != tile.tex_height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
    tile.tex_width = w;
    tile.tex_height = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  tile.uploaded_seq = frame.seq;
}

void CallVideoTileRenderer::DrawTile(Rml::Element* element, GpuTile& tile) {
  if (!element || tile.gl_tex == 0 || tile.uploaded_seq == 0) {
    return;
  }

  Rml::RenderInterface* render_interface = Backend::GetRenderInterface();
  auto* renderer = dynamic_cast<RenderInterface_GL3*>(render_interface);
  if (!renderer) {
    return;
  }

  const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
  const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
  if (size.x <= 0.f || size.y <= 0.f) {
    return;
  }

  // Letterbox / pillarbox into the tile so frames keep their native aspect ratio.
  Rml::Vector2f draw_size = size;
  Rml::Vector2f draw_offset = offset;
  if (tile.tex_width > 0 && tile.tex_height > 0) {
    const float tex_aspect =
        static_cast<float>(tile.tex_width) / static_cast<float>(tile.tex_height);
    const float box_aspect = size.x / size.y;
    if (tex_aspect > box_aspect) {
      draw_size.y = size.x / tex_aspect;
      draw_offset.y = offset.y + (size.y - draw_size.y) * 0.5f;
    } else if (tex_aspect < box_aspect) {
      draw_size.x = size.y * tex_aspect;
      draw_offset.x = offset.x + (size.x - draw_size.x) * 0.5f;
    }
  }

  Rml::Mesh mesh;
  Rml::MeshUtilities::GenerateQuad(mesh, draw_offset, draw_size,
                                   Rml::ColourbPremultiplied(255, 255, 255, 255));
  const Rml::CompiledGeometryHandle handle = renderer->CompileGeometry(mesh.vertices, mesh.indices);
  renderer->RenderGeometry(handle, Rml::Vector2f(0.f, 0.f),
                           static_cast<Rml::TextureHandle>(static_cast<uintptr_t>(tile.gl_tex)));
  renderer->ReleaseGeometry(handle);
}

void CallVideoTileRenderer::RenderTile(CallVideoTileKind kind, Rml::Element* element) {
  GpuTile& tile = (kind == CallVideoTileKind::Local) ? local_ : remote_;
  UploadIfNeeded(tile);
  DrawTile(element, tile);
}

} // namespace pbr
