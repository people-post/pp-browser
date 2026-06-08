#include "TextSelectionController.h"

#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
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

void CollectFlatText(Element* element, String& out, ElementText*& first_text)
{
	for (int i = 0; i < element->GetNumChildren(); ++i)
	{
		Element* child = element->GetChild(i);
		if (auto* text = rmlui_dynamic_cast<ElementText*>(child))
		{
			if (!first_text)
				first_text = text;
			out += text->GetText();
		}
		else if (child->GetNumChildren() > 0)
		{
			if (!out.empty() && out.back() != '\n' && IsBlockTag(child->GetTagName()))
				out += '\n';
			CollectFlatText(child, out, first_text);
			if (IsBlockTag(child->GetTagName()) && (out.empty() || out.back() != '\n'))
				out += '\n';
		}
	}
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

void TextSelectionController::RefreshTextFromContainer()
{
	if (!container)
		return;

	reference_text = nullptr;
	flat_text.clear();
	CollectFlatText(container, flat_text, reference_text);
	while (!flat_text.empty() && flat_text.back() == '\n')
		flat_text.pop_back();
}

void TextSelectionController::ClearSelection()
{
	selection_geometry = {};
	container = nullptr;
	reference_text = nullptr;
	flat_text.clear();
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

	const Vector2f content_origin = container->GetAbsoluteOffset() + container->GetBox().GetPosition(BoxArea::Content);
	const float max_width = container->GetBox().GetSize(BoxArea::Content).x;
	const float line_height = container->GetComputedValues().line_height().value;
	const Style::WhiteSpace white_space = container->GetComputedValues().white_space();
	const bool wrap = (white_space == Style::WhiteSpace::Prewrap || white_space == Style::WhiteSpace::Preline ||
		white_space == Style::WhiteSpace::Normal);

	Vector2f line_position = content_origin;
	int line_begin = 0;

	while (line_begin < (int)flat_text.size())
	{
		int line_end = line_begin;
		if (wrap && max_width > 0.f)
		{
			const char* p_line_start = flat_text.data() + line_begin;
			const char* p_text_end = flat_text.data() + flat_text.size();
			const char* p_hard_break = p_line_start;
			while (p_hard_break < p_text_end && *p_hard_break != '\n')
				++p_hard_break;

			int best_byte_offset = 0;
			for (auto it = StringIteratorU8(p_line_start, p_line_start, p_hard_break); it;)
			{
				++it;
				const int byte_offset = (int)it.offset();
				const float width = float(
					ElementUtilities::GetStringWidth(reference_text, StringView(p_line_start, p_line_start + byte_offset)));
				if (width > max_width)
					break;
				best_byte_offset = byte_offset;
			}

			if (best_byte_offset == 0 && p_line_start < p_hard_break)
			{
				auto it = StringIteratorU8(p_line_start, p_line_start, p_hard_break);
				++it;
				best_byte_offset = (int)it.offset();
			}

			line_end = line_begin + best_byte_offset;
		}
		else
		{
			while (line_end < (int)flat_text.size() && flat_text[line_end] != '\n')
				++line_end;
		}

		if (line_end < (int)flat_text.size() && flat_text[line_end] == '\n')
		{
			LineLayout line;
			line.begin = line_begin;
			line.length = line_end - line_begin;
			line.position = line_position;
			line.height = line_height;
			line.width = float(ElementUtilities::GetStringWidth(reference_text,
				StringView(flat_text.data() + line.begin, flat_text.data() + line_end)));
			lines.push_back(line);
			line_begin = line_end + 1;
		}
		else
		{
			LineLayout line;
			line.begin = line_begin;
			line.length = line_end - line_begin;
			line.position = line_position;
			line.height = line_height;
			line.width = float(ElementUtilities::GetStringWidth(reference_text,
				StringView(flat_text.data() + line.begin, flat_text.data() + line_end)));
			lines.push_back(line);
			line_begin = line_end;
		}

		line_position.y += line_height;
	}
}

void TextSelectionController::SetSelectionIndices(int anchor, int focus)
{
	anchor_index = Math::Clamp(anchor, 0, (int)flat_text.size());
	focus_index = Math::Clamp(focus, 0, (int)flat_text.size());
	BuildSelectionGeometry();
}

int TextSelectionController::HitTest(Vector2f absolute_mouse) const
{
	if (lines.empty() || !reference_text || !reference_text->GetOwnerDocument())
		return 0;

	int best_index = 0;
	float best_distance = Math::SquareRoot(std::numeric_limits<float>::max());

	for (const LineLayout& line : lines)
	{
		if (absolute_mouse.y < line.position.y - line.height || absolute_mouse.y > line.position.y + line.height)
			continue;

		const char* p_begin = flat_text.data() + line.begin;
		const char* p_end = p_begin + line.length;
		int prev_offset = 0;
		float prev_width = 0.f;

		for (auto it = StringIteratorU8(p_begin, p_begin, p_end); it; ++it)
		{
			const int offset = (int)it.offset();
			const float width = float(ElementUtilities::GetStringWidth(reference_text, StringView(p_begin, p_begin + offset)));
			const float distance = Math::Absolute(width - (absolute_mouse.x - line.position.x));
			if (distance < best_distance)
			{
				best_distance = distance;
				best_index = line.begin + offset;
			}
			if (width > absolute_mouse.x - line.position.x)
			{
				const float left_distance = Math::Absolute(width - (absolute_mouse.x - line.position.x));
				const float right_distance = Math::Absolute(prev_width - (absolute_mouse.x - line.position.x));
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
	if (!EnsureContainerAlive() || !reference_text || !reference_text->GetOwnerDocument())
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
		const int line_start = line.begin;
		const int line_end = line.begin + line.length;
		const int sel_start = Math::Clamp(start, line_start, line_end);
		const int sel_end = Math::Clamp(end, line_start, line_end);
		if (sel_start >= sel_end)
			continue;

		const char* p_begin = flat_text.data() + line.begin;
		const float pre_width = float(ElementUtilities::GetStringWidth(reference_text, StringView(p_begin, p_begin + (sel_start - line.begin))));
		const float sel_width = float(ElementUtilities::GetStringWidth(reference_text,
			StringView(p_begin + (sel_start - line.begin), p_begin + (sel_end - line.begin))));

		const Vector2f position = line.position + Vector2f(pre_width, 0.f);
		const Vector2f size(sel_width, line.height);
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
	reference_text = nullptr;
	flat_text.clear();
	CollectFlatText(container, flat_text, reference_text);
	while (!flat_text.empty() && flat_text.back() == '\n')
		flat_text.pop_back();

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
