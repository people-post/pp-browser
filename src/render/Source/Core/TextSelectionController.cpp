#include "TextSelectionController.h"

#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Input.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"

#include <limits>

namespace Rml {

namespace {

bool IsBlockTag(const String& tag)
{
	return tag == "p" || tag == "div" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "li" || tag == "ul" || tag == "ol" ||
		tag == "blockquote" || tag == "pre";
}

void AppendElementText(ElementText* text, String& out)
{
	const ElementText::LineList& layout_lines = text->GetLines();
	if (!layout_lines.empty())
	{
		for (const ElementText::Line& line : layout_lines)
			out += line.text;
	}
	else
	{
		out += text->GetText();
	}
}

void GetLineFontMetrics(ElementText* text_element, float& ascent, float& descent)
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

} // namespace

TextSelectionController::TextSelectionController(Context* context) : context(context) {}

bool TextSelectionController::IsSelectable(const Element* element) const
{
	return element && element->GetAttribute<String>("selectable", "") == "text";
}

Element* TextSelectionController::FindSelectableRoot(Element* hover) const
{
	for (Element* element = hover; element; element = element->GetParentNode())
	{
		if (IsSelectable(element))
			return element;
	}
	return nullptr;
}

bool TextSelectionController::IsFormControl(const Element* element) const
{
	if (!element)
		return false;
	const String& tag = element->GetTagName();
	return tag == "input" || tag == "textarea" || tag == "select" || tag == "button";
}

bool TextSelectionController::ShouldPreventFocus(Element* hover) const
{
	return FindSelectableRoot(hover) != nullptr && !IsFormControl(hover);
}

bool TextSelectionController::ShouldSuppressClick() const
{
	return dragging && anchor_index != focus_index;
}

bool TextSelectionController::EnsureContainerAlive()
{
	if (!container || !container->GetOwnerDocument())
	{
		ClearSelection();
		return false;
	}
	return true;
}

void TextSelectionController::CollectTextFromContainer(Element* element, String& out, ElementText*& first_text)
{
	for (int i = 0; i < element->GetNumChildren(); ++i)
	{
		Element* child = element->GetChild(i);
		if (auto* text = rmlui_dynamic_cast<ElementText*>(child))
		{
			const int begin = (int)out.size();
			AppendElementText(text, out);
			if (begin < (int)out.size())
			{
				if (!first_text)
					first_text = text;
				segments.push_back({text, begin, (int)out.size()});
			}
		}
		else if (child->GetNumChildren() > 0)
		{
			if (!out.empty() && out.back() != '\n' && IsBlockTag(child->GetTagName()))
				out += '\n';
			CollectTextFromContainer(child, out, first_text);
			if (IsBlockTag(child->GetTagName()) && (out.empty() || out.back() != '\n'))
				out += '\n';
		}
	}
}

void TextSelectionController::RefreshTextFromContainer()
{
	if (!container)
		return;

	reference_text = nullptr;
	flat_text.clear();
	segments.clear();
	CollectTextFromContainer(container, flat_text, reference_text);
	while (!flat_text.empty() && flat_text.back() == '\n')
		flat_text.pop_back();
}

void TextSelectionController::ClearSelection()
{
	selection_geometry = {};
	container = nullptr;
	reference_text = nullptr;
	flat_text.clear();
	segments.clear();
	lines.clear();
	anchor_index = 0;
	focus_index = 0;
	dragging = false;
}

void TextSelectionController::RebuildLayout()
{
	lines.clear();
	if (!container || flat_text.empty() || !reference_text)
		return;

	for (const TextSegment& segment : segments)
	{
		ElementText* text_element = segment.element;
		if (!text_element || text_element->GetFontFaceHandle() == 0)
			continue;

		float ascent = 0.f;
		float descent = 0.f;
		GetLineFontMetrics(text_element, ascent, descent);

		const Vector2f text_origin = text_element->GetAbsoluteOffset();
		int flat_cursor = segment.flat_begin;

		const ElementText::LineList& layout_lines = text_element->GetLines();
		if (!layout_lines.empty())
		{
			for (const ElementText::Line& line : layout_lines)
			{
				if (line.text.empty())
					continue;

				LineLayout layout;
				layout.text_element = text_element;
				layout.begin = flat_cursor;
				layout.length = (int)line.text.size();
				layout.baseline = text_origin + line.position;
				layout.width = float(line.width);
				layout.ascent = ascent;
				layout.descent = descent;
				lines.push_back(layout);
				flat_cursor += layout.length;
			}
		}
		else
		{
			const String& raw_text = text_element->GetText();
			if (!raw_text.empty())
			{
				LineLayout layout;
				layout.text_element = text_element;
				layout.begin = flat_cursor;
				layout.length = (int)raw_text.size();
				layout.baseline = text_origin;
				layout.width = float(ElementUtilities::GetStringWidth(text_element, raw_text));
				layout.ascent = ascent;
				layout.descent = descent;
				lines.push_back(layout);
			}
		}
	}
}

int TextSelectionController::HitTest(Vector2f absolute_mouse) const
{
	if (lines.empty())
		return 0;

	int best_index = 0;
	float best_distance = Math::SquareRoot(std::numeric_limits<float>::max());

	for (const LineLayout& line : lines)
	{
		if (!line.text_element)
			continue;

		const float top = line.baseline.y - line.ascent;
		const float bottom = line.baseline.y + line.descent;
		if (absolute_mouse.y < top || absolute_mouse.y > bottom)
			continue;

		const char* p_begin = flat_text.data() + line.begin;
		const char* p_end = p_begin + line.length;
		int prev_offset = 0;
		float prev_width = 0.f;

		for (auto it = StringIteratorU8(p_begin, p_begin, p_end); it; ++it)
		{
			const int offset = (int)it.offset();
			const float width = float(ElementUtilities::GetStringWidth(line.text_element, StringView(p_begin, p_begin + offset)));
			const float distance = Math::Absolute(width - (absolute_mouse.x - line.baseline.x));
			if (distance < best_distance)
			{
				best_distance = distance;
				best_index = line.begin + offset;
			}
			if (width > absolute_mouse.x - line.baseline.x)
			{
				const float left_distance = Math::Absolute(width - (absolute_mouse.x - line.baseline.x));
				const float right_distance = Math::Absolute(prev_width - (absolute_mouse.x - line.baseline.x));
				return line.begin + (left_distance < right_distance ? prev_offset : offset);
			}
			prev_offset = offset;
			prev_width = width;
		}

		best_index = line.begin + line.length;
	}

	return best_index;
}

String TextSelectionController::GetSelectedText() const
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || start >= (int)flat_text.size())
		return {};
	return flat_text.substr(start, end - start);
}

void TextSelectionController::BuildSelectionGeometry()
{
	if (!EnsureContainerAlive())
		return;

	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end)
	{
		selection_geometry = {};
		return;
	}

	RenderManager* render_manager = container->GetRenderManager();
	if (!render_manager)
		return;

	Mesh mesh = selection_geometry.Release(Geometry::ReleaseMode::ClearMesh);
	const ColourbPremultiplied fill(100, 150, 255, 120);

	for (const LineLayout& line : lines)
	{
		if (!line.text_element)
			continue;

		const int line_start = line.begin;
		const int line_end = line.begin + line.length;
		const int sel_start = Math::Clamp(start, line_start, line_end);
		const int sel_end = Math::Clamp(end, line_start, line_end);
		if (sel_start >= sel_end)
			continue;

		const char* p_begin = flat_text.data() + line.begin;
		const float pre_width = float(
			ElementUtilities::GetStringWidth(line.text_element, StringView(p_begin, p_begin + (sel_start - line.begin))));
		const float sel_width = float(ElementUtilities::GetStringWidth(line.text_element,
			StringView(p_begin + (sel_start - line.begin), p_begin + (sel_end - line.begin))));

		const Vector2f position = line.baseline + Vector2f(pre_width, -line.ascent);
		const Vector2f size(sel_width, line.ascent + line.descent);
		MeshUtilities::GenerateQuad(mesh, position, size, fill);
	}

	if (mesh.indices.empty())
		selection_geometry = {};
	else
		selection_geometry = render_manager->MakeGeometry(std::move(mesh));
}

void TextSelectionController::OnMouseDown(Element* hover, Vector2i mouse_position, int key_modifier_state)
{
	(void)key_modifier_state;

	if (IsFormControl(hover))
		return;

	Element* selectable = FindSelectableRoot(hover);
	if (!selectable)
	{
		ClearSelection();
		return;
	}

	container = selectable;
	RefreshTextFromContainer();

	if (flat_text.empty() || !reference_text)
	{
		ClearSelection();
		return;
	}

	RebuildLayout();
	const int index = HitTest(Vector2f(float(mouse_position.x), float(mouse_position.y)));
	anchor_index = index;
	focus_index = index;
	dragging = true;
	BuildSelectionGeometry();
}

void TextSelectionController::OnMouseMove(Vector2i mouse_position)
{
	if (!dragging || !EnsureContainerAlive())
		return;

	focus_index = HitTest(Vector2f(float(mouse_position.x), float(mouse_position.y)));
	BuildSelectionGeometry();
}

void TextSelectionController::OnMouseUp()
{
	if (dragging && EnsureContainerAlive())
	{
		RefreshTextFromContainer();
		if (flat_text.empty() || !reference_text)
		{
			ClearSelection();
			return;
		}
		RebuildLayout();
		BuildSelectionGeometry();
	}
	dragging = false;
}

bool TextSelectionController::OnKeyDown(Input::KeyIdentifier key, int key_modifier_state)
{
	const bool ctrl = (key_modifier_state & Input::KM_CTRL) != 0;
	if (!ctrl || key != Input::KI_C)
		return false;

	const String selected = GetSelectedText();
	if (selected.empty())
		return false;

	if (SystemInterface* system = GetSystemInterface())
		system->SetClipboardText(selected);
	return true;
}

void TextSelectionController::Render()
{
	if (!selection_geometry)
		return;
	selection_geometry.Render(Vector2f(0.f));
}

} // namespace Rml
