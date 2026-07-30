#include "CallVideoTileRenderer.h"

#include "GlBackend.h"
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

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
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
  remote_ = {};
  local_ = {};
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

void CallVideoTileRenderer::DrawTile(Rml::Element* element, GpuTile& tile, RenderInterface_GL3& renderer) {
  if (!element || tile.gl_tex == 0 || tile.uploaded_seq == 0) {
    return;
  }

  const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
  const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
  if (size.x <= 0.f || size.y <= 0.f) {
    return;
  }

  Rml::Mesh mesh;
  Rml::MeshUtilities::GenerateQuad(mesh, offset, size, Rml::ColourbPremultiplied(255, 255, 255, 255));
  const Rml::CompiledGeometryHandle handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
  renderer.RenderGeometry(handle, Rml::Vector2f(0.f, 0.f),
                          static_cast<Rml::TextureHandle>(static_cast<uintptr_t>(tile.gl_tex)));
  renderer.ReleaseGeometry(handle);
}

void CallVideoTileRenderer::Draw(Rml::Context* context, RenderInterface_GL3& renderer) {
  if (!context || context->GetNumDocuments() == 0) {
    return;
  }

  UploadIfNeeded(remote_);
  UploadIfNeeded(local_);

  if (remote_.gl_tex == 0 && local_.gl_tex == 0) {
    return;
  }

  Rml::ElementDocument* document = context->GetDocument(0);
  if (!document) {
    return;
  }

  DrawTile(document->GetElementById("call-remote-tile"), remote_, renderer);
  DrawTile(document->GetElementById("call-local-tile"), local_, renderer);
}

} // namespace pbr
