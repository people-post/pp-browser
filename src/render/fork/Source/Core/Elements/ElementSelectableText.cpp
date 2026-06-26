#include "ElementSelectableText.h"

#include "../../../Include/RmlUi/Core/Context.h"
#include "../../../Include/RmlUi/Core/ElementDocument.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../../Include/RmlUi/Core/Factory.h"
#include "../../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../../Include/RmlUi/Core/RenderManager.h"
#include "../SelectionContentBuilder.h"
#include "../SelectionController.h"
#include "../SelectionHighlight.h"

#include <algorithm>
#include <limits>

namespace Rml {

namespace {

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

SelectionController* GetSelectionController(Element* element)
{
	if (!element)
		return nullptr;
	if (ElementDocument* document = element->GetOwnerDocument())
		if (Context* context = document->GetContext())
			return context->GetSelectionController();
	return nullptr;
}

} // namespace

ElementSelectableText::ElementSelectableText(const String& tag) : Element(tag)
{
	AddEventListener(EventId::Click, this, true);

	ElementPtr unique_selection = Factory::InstanceElement(this, "#selection", "selection", XMLAttributes());
	if (ElementTextSelection* text_selection_element = rmlui_dynamic_cast<ElementTextSelection*>(unique_selection.get()))
	{
		selection_style_element = text_selection_element;
		text_selection_element->SetClient(this);
		AppendChild(std::move(unique_selection), false);
	}
}

ElementSelectableText::~ElementSelectableText()
{
	RemoveEventListener(EventId::Click, this, true);
}

bool ElementSelectableText::IsSelectionRoot() const
{
	for (const Element* parent = GetParentNode(); parent; parent = parent->GetParentNode())
	{
		if (rmlui_dynamic_cast<const ElementSelectableText*>(parent))
			return false;
	}
	return true;
}

void ElementSelectableText::ClearSelectionHighlight()
{
	active_selection_start = 0;
	active_selection_end = 0;
	suppress_click = false;
	ClearSelectionHandles();

	for (const TextSegment& segment : segments)
	{
		if (segment.element)
			segment.element->ClearSelectionHighlight();
	}
}

void ElementSelectableText::ClearSelectionHandles()
{
	handle_start_geometry = {};
	handle_end_geometry = {};
}

Vector2f ElementSelectableText::GetAbsolutePositionForFlatIndex(int flat_index)
{
	if (lines.empty())
		return GetContentRenderOrigin();

	flat_index = Math::Clamp(flat_index, 0, (int)flat_text.size());

	for (const LineLayout& line : lines)
	{
		const int line_end = line.begin + line.length;
		if (flat_index < line.begin || flat_index > line_end)
			continue;

		const int line_local = Math::Min(flat_index - line.begin, line.length);
		float width = 0.f;
		if (line.text_element && line.length > 0)
		{
			const char* p_begin = flat_text.data() + line.begin;
			width = float(ElementUtilities::GetStringWidth(line.text_element, StringView(p_begin, p_begin + line_local)));
		}

		return GetContentRenderOrigin() + line.baseline + Vector2f(width, 0.f);
	}

	const LineLayout& last = lines.back();
	return GetContentRenderOrigin() + last.baseline + Vector2f(last.width, 0.f);
}

void ElementSelectableText::UpdateSelectionHandles(int local_start_index, int local_end_index, bool show_start, bool show_end)
{
	RenderManager* render_manager = GetRenderManager();
	if (!render_manager)
		return;

	const float dp_ratio = GetContext() ? GetContext()->GetDensityIndependentPixelRatio() : 1.f;
	ColourbPremultiplied fill = Colourb(50, 120, 255, 255).ToPremultiplied();
	if (selection_style_element)
	{
		ColourbPremultiplied resolved;
		ResolveSelectionBackground(selection_style_element, resolved, nullptr, SelectionColorFallback::StaticDefault);
		fill = resolved;
	}

	if (show_start)
	{
		const Vector2f absolute = GetAbsolutePositionForFlatIndex(local_start_index);
		const Vector2f local = absolute - GetContentRenderOrigin();
		Mesh mesh = handle_start_geometry.Release(Geometry::ReleaseMode::ClearMesh);
		BuildSelectionHandleGeometry(local, dp_ratio, fill, mesh);
		if (!mesh.indices.empty())
			handle_start_geometry = render_manager->MakeGeometry(std::move(mesh));
	}
	else
	{
		handle_start_geometry = {};
	}

	if (show_end)
	{
		const Vector2f absolute = GetAbsolutePositionForFlatIndex(local_end_index);
		const Vector2f local = absolute - GetContentRenderOrigin();
		Mesh mesh = handle_end_geometry.Release(Geometry::ReleaseMode::ClearMesh);
		BuildSelectionHandleGeometry(local, dp_ratio, fill, mesh);
		if (!mesh.indices.empty())
			handle_end_geometry = render_manager->MakeGeometry(std::move(mesh));
	}
	else
	{
		handle_end_geometry = {};
	}
}

void ElementSelectableText::OnRender()
{
	const Vector2f translation = GetContentRenderOrigin();
	handle_start_geometry.Render(translation);
	handle_end_geometry.Render(translation);
}

SelectionDisposition ElementSelectableText::QuerySelection(const SelectionQuery& query)
{
	if (query.phase == SelectionQuery::Phase::PointerDown)
		return SelectionDisposition::Participate;
	return SelectionDisposition::Default;
}

void ElementSelectableText::BuildSelectionContent(SelectionContentBuilder& builder)
{
	builder.BeginContainer(this);
	for (int i = 0; i < GetNumChildren(); ++i)
	{
		Element* child = GetChild(i);
		if (!child || child == selection_style_element)
			continue;
		child->BuildSelectionContent(builder);
	}
	builder.EndContainer();
}

void ElementSelectableText::RefreshSelectionContent()
{
	SelectionContentBuilder builder;
	BuildSelectionContent(builder);
	flat_text = builder.GetFlatText();
	segments.clear();
	for (const SelectionContentBuilder::TextSegment& segment : builder.GetSegments())
		segments.push_back({segment.element, segment.flat_begin, segment.flat_end});
	reference_text = segments.empty() ? nullptr : segments.front().element;
	while (!flat_text.empty() && flat_text.back() == '\n')
		flat_text.pop_back();

	for (TextSegment& segment : segments)
	{
		if (segment.flat_end > (int)flat_text.size())
			segment.flat_end = (int)flat_text.size();
	}

	RebuildLayout();
}

SelectionEndpoint ElementSelectableText::HitTestSelection(Vector2f absolute_position) const
{
	SelectionEndpoint endpoint;
	endpoint.owner = const_cast<ElementSelectableText*>(this);
	endpoint.index = const_cast<ElementSelectableText*>(this)->HitTestLocal(absolute_position);
	return endpoint;
}

int ElementSelectableText::HitTestLocal(Vector2f absolute_mouse)
{
	if (lines.empty())
		return 0;

	const Vector2f local_mouse = absolute_mouse - GetContentRenderOrigin();

	int best_index = 0;
	float best_distance = Math::SquareRoot(std::numeric_limits<float>::max());

	for (const LineLayout& line : lines)
	{
		if (!line.text_element)
			continue;

		const float top = line.baseline.y - line.ascent;
		const float bottom = line.baseline.y + line.descent;
		if (local_mouse.y < top || local_mouse.y > bottom)
			continue;

		const char* p_begin = flat_text.data() + line.begin;
		const char* p_end = p_begin + line.length;
		int prev_offset = 0;
		float prev_width = 0.f;

		for (auto it = StringIteratorU8(p_begin, p_begin, p_end); it; ++it)
		{
			const int offset = (int)it.offset();
			const float width = float(ElementUtilities::GetStringWidth(line.text_element, StringView(p_begin, p_begin + offset)));
			const float distance = Math::Absolute(width - (local_mouse.x - line.baseline.x));
			if (distance < best_distance)
			{
				best_distance = distance;
				best_index = line.begin + offset;
			}
			if (width > local_mouse.x - line.baseline.x)
			{
				const float left_distance = Math::Absolute(width - (local_mouse.x - line.baseline.x));
				const float right_distance = Math::Absolute(prev_width - (local_mouse.x - line.baseline.x));
				return line.begin + (left_distance < right_distance ? prev_offset : offset);
			}
			prev_offset = offset;
			prev_width = width;
		}

		best_index = line.begin + line.length;
	}

	return best_index;
}

String ElementSelectableText::GetSelectionSlice(int local_start, int local_end) const
{
	const int start = Math::Min(local_start, local_end);
	const int end = Math::Max(local_start, local_end);
	if (start >= end || start >= (int)flat_text.size())
		return {};
	return flat_text.substr(start, end - start);
}

void ElementSelectableText::UpdateSelectionHighlight(int local_start, int local_end)
{
	active_selection_start = Math::Min(local_start, local_end);
	active_selection_end = Math::Max(local_start, local_end);

	for (const TextSegment& segment : segments)
	{
		if (segment.element)
			segment.element->ClearSelectionHighlight();
	}

	if (active_selection_start >= active_selection_end)
		return;

	for (const TextSegment& segment : segments)
	{
		if (!segment.element)
			continue;

		const int slice_start =
			Math::Clamp(active_selection_start, segment.flat_begin, segment.flat_end) - segment.flat_begin;
		const int slice_end = Math::Clamp(active_selection_end, segment.flat_begin, segment.flat_end) - segment.flat_begin;
		if (slice_start < slice_end)
			segment.element->RenderSelectionSlice(slice_start, slice_end);
	}
}

void ElementSelectableText::OnSelectionStyleChanged()
{
	RebuildActiveSelectionHighlight();
}

void ElementSelectableText::RebuildActiveSelectionHighlight()
{
	if (active_selection_start < active_selection_end)
		UpdateSelectionHighlight(active_selection_start, active_selection_end);
}

Vector2f ElementSelectableText::GetContentRenderOrigin()
{
	return GetAbsoluteOffset() - Vector2f(GetScrollLeft(), GetScrollTop());
}

void ElementSelectableText::RebuildLayout()
{
	lines.clear();
	if (flat_text.empty() || !reference_text)
		return;

	const Vector2f render_origin = GetContentRenderOrigin();

	for (const TextSegment& segment : segments)
	{
		ElementText* text_element = segment.element;
		if (!text_element || text_element->GetFontFaceHandle() == 0)
			continue;

		float ascent = 0.f;
		float descent = 0.f;
		GetLineFontMetrics(text_element, ascent, descent);

		const Vector2f text_origin = text_element->GetAbsoluteOffset() - render_origin;
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

void ElementSelectableText::ProcessEvent(Event& event)
{
	Element* target = event.GetTargetElement();
	if (!target || !Contains(target))
		return;

	if (event == EventId::Click && event.GetPhase() == EventPhase::Capture)
	{
		if (SelectionController* controller = GetSelectionController(this))
		{
			if (controller->ShouldSuppressClick(target))
				event.StopPropagation();
		}
	}
}

} // namespace Rml
