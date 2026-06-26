#include "SelectionHighlight.h"

#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/RenderManager.h"

namespace Rml {

namespace {

constexpr byte kStaticSelectionRed = 180;
constexpr byte kStaticSelectionGreen = 213;
constexpr byte kStaticSelectionBlue = 254;

void GetTextFontMetrics(ElementText* text_element, float& ascent, float& descent)
{
	ascent = 0.f;
	descent = 0.f;
	if (!text_element)
		return;

	const FontFaceHandle font_face_handle = text_element->GetFontFaceHandle();
	if (font_face_handle == 0)
		return;

	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(font_face_handle);
	ascent = float(Math::RoundUpToInteger(metrics.ascent));
	descent = float(Math::RoundUpToInteger(metrics.descent));
}

String GetTextContent(const ElementText* text_element)
{
	String content;
	const ElementText::LineList& layout_lines = text_element->GetLines();
	if (!layout_lines.empty())
	{
		for (const ElementText::Line& line : layout_lines)
			content += line.text;
	}
	else
	{
		content = text_element->GetText();
	}
	return content;
}

} // namespace

Element* FindSelectionStyleElement(Element* style_root)
{
	if (!style_root)
		return nullptr;

	for (int i = 0; i < style_root->GetNumChildren(); ++i)
	{
		Element* child = style_root->GetChild(i);
		if (child && child->GetTagName() == "selection")
			return child;
	}
	return nullptr;
}

void ResolveSelectionBackground(Element* style_root, ColourbPremultiplied& fill, Colourb* selected_text_color,
	SelectionColorFallback fallback)
{
	if (!style_root)
	{
		fill = Colourb(kStaticSelectionRed, kStaticSelectionGreen, kStaticSelectionBlue, 255).ToPremultiplied();
		return;
	}

	Element* selection_element = FindSelectionStyleElement(style_root);

	Colourb resolved_text_color;
	if (selected_text_color)
	{
		const Property* colour_property = selection_element ? selection_element->GetLocalProperty(PropertyId::Color) : nullptr;
		if (colour_property)
		{
			resolved_text_color = colour_property->Get<Colourb>();
		}
		else if (fallback == SelectionColorFallback::EditorInverse)
		{
			resolved_text_color = style_root->GetComputedValues().color();
			resolved_text_color.red = byte(255 - resolved_text_color.red);
			resolved_text_color.green = byte(255 - resolved_text_color.green);
			resolved_text_color.blue = byte(255 - resolved_text_color.blue);
		}
		else
		{
			resolved_text_color = style_root->GetComputedValues().color();
		}
		*selected_text_color = resolved_text_color;
	}

	const Property* background_property =
		selection_element ? selection_element->GetLocalProperty(PropertyId::BackgroundColor) : nullptr;
	if (background_property)
	{
		fill = background_property->Get<Colourb>().ToPremultiplied();
		return;
	}

	if (fallback == SelectionColorFallback::EditorInverse && selected_text_color)
	{
		Colourb background(255 - selected_text_color->red, 255 - selected_text_color->green, 255 - selected_text_color->blue,
			selected_text_color->alpha);
		fill = background.ToPremultiplied();
		return;
	}

	fill = Colourb(kStaticSelectionRed, kStaticSelectionGreen, kStaticSelectionBlue, 255).ToPremultiplied();
}

void AppendSelectionQuad(Mesh& mesh, Vector2f position, Vector2f size, ColourbPremultiplied fill)
{
	MeshUtilities::GenerateQuad(mesh, position, size, fill);
}

void BuildSelectionHandleGeometry(Vector2f head_center, float dp_ratio, ColourbPremultiplied fill, Mesh& mesh)
{
	const float head_radius = 6.f * dp_ratio;
	const float stem_width = 2.5f * dp_ratio;
	const float stem_length = 8.f * dp_ratio;

	const Vector2f stem_origin(head_center.x - stem_width * 0.5f, head_center.y);
	MeshUtilities::GenerateQuad(mesh, stem_origin, Vector2f(stem_width, stem_length), fill);

	constexpr int segments = 16;
	const int base_index = int(mesh.vertices.size());
	mesh.vertices.resize(base_index + segments + 1);
	mesh.vertices[base_index].position = head_center;
	mesh.vertices[base_index].colour = fill;

	for (int i = 0; i < segments; ++i)
	{
		const float angle = float(i) / float(segments) * Math::RMLUI_PI * 2.f;
		Vertex& vertex = mesh.vertices[base_index + 1 + i];
		vertex.position = head_center + Vector2f(Math::Cos(angle) * head_radius, Math::Sin(angle) * head_radius);
		vertex.colour = fill;
	}

	for (int i = 0; i < segments; ++i)
	{
		mesh.indices.push_back(base_index);
		mesh.indices.push_back(base_index + 1 + i);
		mesh.indices.push_back(base_index + 1 + ((i + 1) % segments));
	}
}

void RenderSelectionHandleDebugMarker(RenderManager& render_manager, Vector2f absolute_center, float dp_ratio)
{
#if defined(RMLUI_DEBUG_SELECTION_HANDLES)
	const float r = 14.f * dp_ratio;
	Mesh mesh;
	const ColourbPremultiplied fill = Colourb(255, 0, 255, 255).ToPremultiplied();
	MeshUtilities::GenerateQuad(mesh, absolute_center - Vector2f(r, r), Vector2f(2.f * r, 2.f * r), fill);
	MeshUtilities::GenerateQuad(mesh, absolute_center + Vector2f(-r, -2.f * dp_ratio), Vector2f(2.f * r, 4.f * dp_ratio),
		Colourb(255, 255, 0, 255).ToPremultiplied());
	render_manager.MakeGeometry(std::move(mesh)).Render({});
#else
	(void)render_manager;
	(void)absolute_center;
	(void)dp_ratio;
#endif
}

void RenderSelectionHandleGeometry(const Geometry& geometry, Vector2f translation)
{
	if (geometry)
		geometry.Render(translation.Round());
}

bool BuildTextSelectionGeometry(ElementText* text_element, int local_start, int local_end, ColourbPremultiplied fill,
	Geometry& out_geometry, RenderManager& render_manager)
{
	const int start = Math::Min(local_start, local_end);
	const int end = Math::Max(local_start, local_end);
	if (!text_element || start >= end)
	{
		out_geometry = {};
		return false;
	}

	const String content = GetTextContent(text_element);
	if (start >= (int)content.size())
	{
		out_geometry = {};
		return false;
	}

	float ascent = 0.f;
	float descent = 0.f;
	GetTextFontMetrics(text_element, ascent, descent);

	Mesh mesh = out_geometry.Release(Geometry::ReleaseMode::ClearMesh);
	const ElementText::LineList& layout_lines = text_element->GetLines();

	if (!layout_lines.empty())
	{
		int flat_cursor = 0;
		for (const ElementText::Line& line : layout_lines)
		{
			if (line.text.empty())
				continue;

			const int line_start = flat_cursor;
			const int line_end = flat_cursor + (int)line.text.size();
			const int sel_start = Math::Clamp(start, line_start, line_end);
			const int sel_end = Math::Clamp(end, line_start, line_end);
			flat_cursor = line_end;

			if (sel_start >= sel_end)
				continue;

			const int line_local_start = sel_start - line_start;
			const int line_local_end = sel_end - line_start;
			const char* p_begin = line.text.c_str();
			const float pre_width =
				float(ElementUtilities::GetStringWidth(text_element, StringView(p_begin, p_begin + line_local_start)));
			const float sel_width = float(ElementUtilities::GetStringWidth(text_element,
				StringView(p_begin + line_local_start, p_begin + line_local_end)));

			const Vector2f position = line.position + Vector2f(pre_width, -ascent);
			const Vector2f size(sel_width, ascent + descent);
			AppendSelectionQuad(mesh, position, size, fill);
		}
	}
	else if (!content.empty())
	{
		const int sel_start = Math::Clamp(start, 0, (int)content.size());
		const int sel_end = Math::Clamp(end, 0, (int)content.size());
		if (sel_start < sel_end)
		{
			const char* p_begin = content.c_str();
			const float pre_width =
				float(ElementUtilities::GetStringWidth(text_element, StringView(p_begin, p_begin + sel_start)));
			const float sel_width = float(
				ElementUtilities::GetStringWidth(text_element, StringView(p_begin + sel_start, p_begin + sel_end)));
			const Vector2f position(pre_width, -ascent);
			const Vector2f size(sel_width, ascent + descent);
			AppendSelectionQuad(mesh, position, size, fill);
		}
	}

	if (mesh.indices.empty())
	{
		out_geometry = {};
		return false;
	}

	out_geometry = render_manager.MakeGeometry(std::move(mesh));
	return true;
}

} // namespace Rml
