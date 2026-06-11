#include "ElementSelectableText.h"

#include "../../../Include/RmlUi/Core/Context.h"
#include "../../../Include/RmlUi/Core/Core.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../../Include/RmlUi/Core/RenderManager.h"
#include "../../../Include/RmlUi/Core/SystemInterface.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"

#include <algorithm>
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

Vector<ElementSelectableText*>& GetInstances()
{
	static Vector<ElementSelectableText*> instances;
	return instances;
}

} // namespace

ElementSelectableText* ElementSelectableText::active_dragger = nullptr;

ElementSelectableText::ElementSelectableText(const String& tag) : Element(tag)
{
	AddEventListener(EventId::Mousedown, this, true);
	AddEventListener(EventId::Mouseup, this, true);
	AddEventListener(EventId::Click, this, true);
	RegisterInstance();
}

ElementSelectableText::~ElementSelectableText()
{
	if (active_dragger == this)
		active_dragger = nullptr;
	UnregisterInstance();
	RemoveEventListener(EventId::Mousedown, this, true);
	RemoveEventListener(EventId::Mouseup, this, true);
	RemoveEventListener(EventId::Click, this, true);
}

void ElementSelectableText::RegisterInstance()
{
	GetInstances().push_back(this);
}

void ElementSelectableText::UnregisterInstance()
{
	auto& instances = GetInstances();
	instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end());
}

bool ElementSelectableText::IsInteractiveElement(const Element* element)
{
	for (const Element* current = element; current; current = current->GetParentNode())
	{
		const String& tag = current->GetTagName();
		if (tag == "input" || tag == "textarea" || tag == "select" || tag == "button" || tag == "a")
			return true;
		if (current->HasAttribute("data-event-click"))
			return true;
	}
	return false;
}

void ElementSelectableText::ClearSelection()
{
	selection_geometry = {};
	flat_text.clear();
	segments.clear();
	lines.clear();
	reference_text = nullptr;
	anchor_index = 0;
	focus_index = 0;
	dragging = false;
	suppress_click = false;
	if (active_dragger == this)
		active_dragger = nullptr;
}

bool ElementSelectableText::HasNonEmptySelection() const
{
	return anchor_index != focus_index && !flat_text.empty();
}

void ElementSelectableText::CollectTextFromContainer(Element* element, String& out, ElementText*& first_text)
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

void ElementSelectableText::RefreshTextFromContainer()
{
	reference_text = nullptr;
	flat_text.clear();
	segments.clear();
	CollectTextFromContainer(this, flat_text, reference_text);
	while (!flat_text.empty() && flat_text.back() == '\n')
		flat_text.pop_back();
}

void ElementSelectableText::RebuildLayout()
{
	lines.clear();
	if (flat_text.empty() || !reference_text)
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

int ElementSelectableText::HitTest(Vector2f absolute_mouse) const
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

String ElementSelectableText::GetSelectedText() const
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || start >= (int)flat_text.size())
		return {};
	return flat_text.substr(start, end - start);
}

void ElementSelectableText::BuildSelectionGeometry()
{
	const int start = Math::Min(anchor_index, focus_index);
	const int end = Math::Max(anchor_index, focus_index);
	if (start >= end || !GetOwnerDocument())
	{
		selection_geometry = {};
		return;
	}

	RenderManager* render_manager = GetRenderManager();
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

void ElementSelectableText::BeginSelection(Vector2i mouse_position)
{
	for (ElementSelectableText* other : GetInstances())
	{
		if (other != this)
			other->ClearSelection();
	}

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
	suppress_click = false;
	active_dragger = this;
	BuildSelectionGeometry();
}

void ElementSelectableText::ExtendSelection(Vector2i mouse_position)
{
	if (!dragging || !GetOwnerDocument())
		return;

	RebuildLayout();
	focus_index = HitTest(Vector2f(float(mouse_position.x), float(mouse_position.y)));
	BuildSelectionGeometry();
}

void ElementSelectableText::EndSelection()
{
	if (dragging && GetOwnerDocument())
	{
		RefreshTextFromContainer();
		if (flat_text.empty() || !reference_text)
		{
			ClearSelection();
			return;
		}
		RebuildLayout();
		BuildSelectionGeometry();
		suppress_click = (anchor_index != focus_index);
	}
	dragging = false;
	if (active_dragger == this)
		active_dragger = nullptr;
}

void ElementSelectableText::ProcessEvent(Event& event)
{
	Element* target = event.GetTargetElement();
	if (!target || !Contains(target))
		return;

	if (event == EventId::Mousedown && event.GetPhase() == EventPhase::Capture)
	{
		if (IsInteractiveElement(target))
		{
			ClearSelection();
			return;
		}

		const int mouse_x = static_cast<int>(event.GetParameter("mouse_x", 0.f));
		const int mouse_y = static_cast<int>(event.GetParameter("mouse_y", 0.f));
		BeginSelection(Vector2i(mouse_x, mouse_y));
		return;
	}

	if (event == EventId::Mouseup && event.GetPhase() == EventPhase::Capture)
	{
		EndSelection();
		return;
	}

	if (event == EventId::Click && event.GetPhase() == EventPhase::Capture && suppress_click && !IsInteractiveElement(target))
		event.StopPropagation();
}

void ElementSelectableText::OnRender()
{
	if (selection_geometry)
		selection_geometry.Render(Vector2f(0.f));
}

void ElementSelectableText::NotifyGlobalMouseMove(Vector2i mouse_position)
{
	if (active_dragger)
		active_dragger->ExtendSelection(mouse_position);
}

void ElementSelectableText::NotifyGlobalMouseUp()
{
	if (active_dragger)
		active_dragger->EndSelection();
}

bool ElementSelectableText::NotifyGlobalKeyDown(Input::KeyIdentifier key, int key_modifier_state)
{
	const bool ctrl = (key_modifier_state & Input::KM_CTRL) != 0;
	if (!ctrl || key != Input::KI_C)
		return false;

	for (ElementSelectableText* instance : GetInstances())
	{
		if (!instance->GetOwnerDocument())
			continue;
		const String selected = instance->GetSelectedText();
		if (selected.empty())
			continue;
		if (SystemInterface* system = GetSystemInterface())
			system->SetClipboardText(selected);
		return true;
	}
	return false;
}

bool ElementSelectableText::IsAnyDragging()
{
	return active_dragger != nullptr;
}

void ElementSelectableText::ClearSelectionsUnlessContaining(Element* hover)
{
	if (hover)
	{
		for (Element* element = hover; element; element = element->GetParentNode())
		{
			if (auto* selectable = rmlui_dynamic_cast<ElementSelectableText*>(element))
			{
				if (!IsInteractiveElement(hover))
					return;
				break;
			}
		}
	}

	for (ElementSelectableText* instance : GetInstances())
		instance->ClearSelection();
}

} // namespace Rml
