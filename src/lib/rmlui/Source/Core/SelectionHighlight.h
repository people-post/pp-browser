#pragma once

#include "../../Include/RmlUi/Core/Colour.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Header.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class ElementText;
class RenderManager;

enum class SelectionColorFallback {
	EditorInverse,
	StaticDefault,
};

/// Locate the hidden RCSS style-probe child (tag "selection") on @p style_root.
Element* FindSelectionStyleElement(Element* style_root);

/// Resolve selection background fill; optionally resolve selected-text foreground for editors.
void ResolveSelectionBackground(Element* style_root, ColourbPremultiplied& fill, Colourb* selected_text_color,
	SelectionColorFallback fallback);

/// Build highlight quads for a character range on @p text_element (local indices into its content).
bool BuildTextSelectionGeometry(ElementText* text_element, int local_start, int local_end, ColourbPremultiplied fill,
	Geometry& out_geometry, RenderManager& render_manager);

/// Append a single selection background quad to an existing mesh.
void AppendSelectionQuad(Mesh& mesh, Vector2f position, Vector2f size, ColourbPremultiplied fill);

/// Build lollipop handle geometry (circle head + stem) centered on @p head_center (document coordinates).
void BuildSelectionHandleGeometry(Vector2f head_center, float dp_ratio, ColourbPremultiplied fill, Mesh& mesh);

/// Debug: draw a high-contrast marker at a handle anchor (requires RMLUI_DEBUG_SELECTION_HANDLES).
void RenderSelectionHandleDebugMarker(RenderManager& render_manager, Vector2f absolute_center, float dp_ratio);

/// Render @p geometry in document space (mesh vertices already absolute; @p translation is typically zero).
void RenderSelectionHandleGeometry(const Geometry& geometry, Vector2f translation);

} // namespace Rml
