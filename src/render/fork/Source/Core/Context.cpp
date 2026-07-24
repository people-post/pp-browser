#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ContextInstancer.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/DataModelHandle.h"
#include "../../Include/RmlUi/Core/Debug.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "ClickRouting.h"
#include "DataModel.h"
#include "EventDispatcher.h"
#include "PluginRegistry.h"
#include "SelectionContentBuilder.h"
#include "SelectionController.h"
#include "ScrollController.h"
#include "StreamFile.h"
#include <algorithm>
#include <clocale>
#include <iterator>
#include <limits>

namespace Rml {

static constexpr float DOUBLE_CLICK_TIME = 0.5f;    // [s]
static constexpr float DOUBLE_CLICK_MAX_DIST = 3.f; // [dp]
static constexpr float UNIT_SCROLL_LENGTH = 80.f;   // [dp]

// If the user stops scrolling for this amount of time in seconds before touch/click release, don't apply inertia.
static constexpr float SCROLL_INERTIA_DELAY = 0.1f;
static constexpr float TOUCH_CLICK_MAX_DISTANCE = DOUBLE_CLICK_MAX_DIST; // [dp]
static constexpr float TOUCH_SCROLL_SLOP = 10.f;                         // [dp]
static constexpr double TOUCH_LONG_PRESS_TIME = 0.5;                   // [s]
// Recent touch samples used to estimate fling velocity (more stable than a single exponential window).
static constexpr float TOUCH_VELOCITY_WINDOW = 0.1f; // [s]

static bool IsContextMenuTarget(Element* element)
{
	for (Element* current = element; current; current = current->GetParentNode())
	{
		if (current->GetId() == "context-menu-layer")
			return true;
	}
	return false;
}

static bool IsTextEditorTarget(Element* element)
{
	for (Element* current = element; current; current = current->GetParentNode())
	{
		const String& tag = current->GetTagName();
		if (tag == "textarea" || tag == "input" || tag == "select")
			return true;
	}
	return false;
}

static String GetEffectiveCursor(Element* hover)
{
	for (Element* element = hover; element; element = element->GetParentNode())
	{
		const String& cursor = element->GetComputedValues().cursor();
		if (!cursor.empty() && cursor != "auto")
			return cursor;

		const String& tag = element->GetTagName();
		if (tag == "button" || tag == "a")
			return "pointer";
		if (tag == "textarea" || tag == "input")
			return "text";
	}
	return {};
}

static bool IsScrollbarRoot(const Element* element)
{
	if (!element)
		return false;
	const String& tag = element->GetTagName();
	return tag == "scrollbarvertical" || tag == "scrollbarhorizontal" || tag == "scrollbarcorner";
}

static bool IsScrollbarElement(const Element* element)
{
	for (const Element* current = element; current; current = current->GetParentNode())
	{
		if (IsScrollbarRoot(current))
			return true;
	}
	return false;
}

static Element* FindScrollbarRoot(Element* element)
{
	for (Element* current = element; current; current = current->GetParentNode())
	{
		if (IsScrollbarRoot(current))
			return current;
	}
	return nullptr;
}

/// When a scrollbar's layout box overlaps content, prefer the content under the pointer.
static Element* PreferContentOverScrollbar(Context* context, Vector2f point, Element* hover)
{
	if (!hover || !IsScrollbarElement(hover))
		return hover;

	Element* scrollbar_root = FindScrollbarRoot(hover);
	if (!scrollbar_root)
		return hover;

	if (Element* content = context->GetElementAtPoint(point, scrollbar_root))
		return content;

	return hover;
}

#ifdef RMLUI_DEBUG
static const char* ElementTagOrNull(Element* element)
{
	return element ? element->GetTagName().c_str() : "null";
}

static void LogPointerInteraction(const char* phase, Element* hover, Element* interactive, Element* active, bool suppress_click,
	bool click_dispatched)
{
	Log::Message(Log::LT_DEBUG, "Pointer %s: hover=%s interactive=%s active=%s suppress_click=%d click=%d", phase, ElementTagOrNull(hover),
		ElementTagOrNull(interactive), ElementTagOrNull(active), suppress_click ? 1 : 0, click_dispatched ? 1 : 0);
}
#endif

static void DebugVerifyLocaleSetting()
{
#ifdef RMLUI_DEBUG
	constexpr float expected_value = 1000.5f;
	const Rml::String expected_string = "1000.5";
	const Rml::String formatted_string = Rml::ToString(expected_value);
	const float parsed_value = Rml::FromString<float>(expected_string);

	const char* description = "RmlUi expects the global locale to be set to the default minimal \"C\" locale, please see `std::setlocale`.";
	if (formatted_string != expected_string)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while formatting %f. Formatted: \"%s\". Expected: \"%s\". Current locale: %s. %s", expected_value,
			formatted_string.c_str(), expected_string.c_str(), std::setlocale(LC_ALL, nullptr), description);
	}
	if (parsed_value != expected_value)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR,
			"Incompatible locale setting detected while parsing \"%s\". Parsed: %f. Expected: %f. Current locale: %s. %s", expected_string.c_str(),
			parsed_value, expected_value, std::setlocale(LC_ALL, nullptr), description);
	}
#endif
}

Context::Context(const String& name, RenderManager* render_manager, TextInputHandler* text_input_handler) :
	name(name), render_manager(render_manager), text_input_handler(text_input_handler)
{
	instancer = nullptr;

	root = Factory::InstanceElement(nullptr, "*", "#root", XMLAttributes());
	root->SetId(name);
	root->SetOffset(Vector2f(0, 0), nullptr);
	root->SetProperty(PropertyId::ZIndex, Property(0, Unit::NUMBER));

	cursor_proxy = Factory::InstanceElement(nullptr, documents_base_tag, documents_base_tag, XMLAttributes());
	ElementDocument* cursor_proxy_document = rmlui_dynamic_cast<ElementDocument*>(cursor_proxy.get());
	RMLUI_ASSERT(cursor_proxy_document);
	cursor_proxy_document->context = this;

	// The cursor proxy takes the style from its cloned element's document. The latter may define style rules for `<body>` which we don't want on the
	// proxy. Thus, we override some properties here that we in particular don't want to inherit from the client document, especially those that
	// result in decoration of the body element.
	cursor_proxy_document->SetProperty(PropertyId::BackgroundColor, Property(Colourb(255, 255, 255, 0), Unit::COLOUR));
	cursor_proxy_document->SetProperty(PropertyId::BorderTopWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderRightWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderBottomWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::BorderLeftWidth, Property(0, Unit::PX));
	cursor_proxy_document->SetProperty(PropertyId::Decorator, Property());
	cursor_proxy_document->SetProperty(PropertyId::OverflowX, Property(Style::Overflow::Visible));
	cursor_proxy_document->SetProperty(PropertyId::OverflowY, Property(Style::Overflow::Visible));

	document_focus_history.push_back(root.get());
	focus = root.get();
	hover = nullptr;
	active = nullptr;
	drag = nullptr;

	drag_started = false;
	drag_verbose = false;
	drag_clone = nullptr;
	drag_hover = nullptr;
	last_click_element = nullptr;
	last_click_time = 0;
	last_click_count = 0;

	mouse_active = false;
	enable_cursor = true;

	scroll_controller = MakeUnique<ScrollController>();
	selection_controller = MakeUnique<SelectionController>(this);
}

Context::~Context()
{
	PluginRegistry::NotifyContextDestroy(this);

	UnloadAllDocuments();

	ReleaseUnloadedDocuments();

	root.reset();

	cursor_proxy.reset();

	instancer = nullptr;
}

const String& Context::GetName() const
{
	return name;
}

void Context::SetDimensions(const Vector2i _dimensions)
{
	if (dimensions != _dimensions)
	{
		dimensions = _dimensions;
		render_manager->SetViewport(dimensions);
		root->SetBox(Box(Vector2f(dimensions)));
		root->DirtyLayout();

		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document != nullptr)
			{
				document->DirtyMediaQueries();
				document->DirtyVwAndVhProperties();
				document->DirtyLayout();
				document->DirtyPosition();
				document->DispatchEvent(EventId::Resize, Dictionary());
			}
		}
	}
}

Vector2i Context::GetDimensions() const
{
	return dimensions;
}

void Context::SetDensityIndependentPixelRatio(float dp_ratio)
{
	if (density_independent_pixel_ratio != dp_ratio)
	{
		density_independent_pixel_ratio = dp_ratio;

		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document)
			{
				document->DirtyMediaQueries();
				document->OnDpRatioChangeRecursive();
			}
		}
	}
}

float Context::GetDensityIndependentPixelRatio() const
{
	return density_independent_pixel_ratio;
}

bool Context::Update()
{
	RMLUI_ZoneScoped;
	DebugVerifyLocaleSetting();

	next_update_timeout = std::numeric_limits<double>::infinity();

	if (scroll_controller->Update(mouse_position, density_independent_pixel_ratio))
		RequestNextUpdate(0);

	UpdateTouchGestures();

	// Update the hover chain to detect any new or moved elements under the mouse.
	if (mouse_active)
		UpdateHoverChain(mouse_position);

	// Update all the data models before updating properties and layout.
	for (auto& data_model : data_models)
		data_model.second->Update(true);

	// The style definition of each document should be independent of each other. By manually resetting these flags we avoid unnecessary definition
	// lookups in unrelated documents, such as when adding a new document. Adding an element dirties the parent definition, which in this case is the
	// root. By extension the definition of all the other documents are also dirtied, unnecessarily.
	root->dirty_definition = false;
	root->dirty_child_definitions = false;

	root->Update(density_independent_pixel_ratio, Vector2f(dimensions));

	for (int i = 0; i < root->GetNumChildren(); ++i)
	{
		if (auto doc = root->GetChild(i)->GetOwnerDocument())
		{
			doc->UpdateLayout();
			doc->UpdatePosition();
		}
	}

	// Rubber-band overscroll is applied unclamped; layout may clamp scroll offsets — restore for this frame.
	scroll_controller->RestoreOverscrollAfterLayout();

	// Data-bound views (e.g. chat message list) may add or move elements during Update(); refresh
	// hover and cursor now that layout reflects the new DOM.
	if (mouse_active)
		UpdateHoverChain(mouse_position);

	// Release any documents that were unloaded during the update.
	ReleaseUnloadedDocuments();

	return true;
}

bool Context::Render()
{
	RMLUI_ZoneScoped;

	render_manager->PrepareRender(dimensions);

	root->Render();

	const TextLoupeState text_loupe_state = GetTextLoupeState();
	if (text_loupe_state.active && text_loupe_render_callback)
		text_loupe_render_callback(TextLoupePhase::Capture, text_loupe_state, *render_manager);

	selection_controller->RenderSelectionHandles();

	// Render the cursor proxy so that any attached drag clone will be rendered below the cursor.
	if (drag_clone)
	{
		static_cast<ElementDocument&>(*cursor_proxy).UpdateDocument();
		cursor_proxy->SetOffset(
			Vector2f((float)Math::Clamp(mouse_position.x, 0, dimensions.x), (float)Math::Clamp(mouse_position.y, 0, dimensions.y)), nullptr);
		cursor_proxy->Render();
	}

	if (text_loupe_state.active && text_loupe_render_callback)
		text_loupe_render_callback(TextLoupePhase::Draw, text_loupe_state, *render_manager);

	render_manager->ResetState();

	return true;
}

ElementDocument* Context::CreateDocument(const String& instancer_name)
{
	ElementPtr element = Factory::InstanceElement(nullptr, instancer_name, documents_base_tag, XMLAttributes());
	if (!element)
	{
		Log::Message(Log::LT_ERROR, "Failed to instance document on instancer_name '%s', instancer returned nullptr.", instancer_name.c_str());
		return nullptr;
	}

	ElementDocument* document = rmlui_dynamic_cast<ElementDocument*>(element.get());
	if (!document)
	{
		Log::Message(Log::LT_ERROR,
			"Failed to instance document on instancer_name '%s', Found type '%s', was expecting derivative of ElementDocument.",
			instancer_name.c_str(), rmlui_type_name(*element));
		return nullptr;
	}

	document->context = this;
	root->AppendChild(std::move(element));

	PluginRegistry::NotifyDocumentLoad(document);

	return document;
}

ElementDocument* Context::LoadDocument(const String& document_path)
{
	auto stream = MakeUnique<StreamFile>();

	if (!stream->Open(document_path))
		return nullptr;

	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

ElementDocument* Context::LoadDocument(Stream* stream)
{
	DebugVerifyLocaleSetting();
	PluginRegistry::NotifyDocumentOpen(this, stream->GetSourceURL().GetURL());

	ElementPtr element = Factory::InstanceDocumentStream(this, stream, GetDocumentsBaseTag());
	if (!element)
		return nullptr;

	ElementDocument* document = rmlui_static_cast<ElementDocument*>(element.get());

	root->AppendChild(std::move(element));

	// The 'load' event is fired before updating the document, because the user might
	// need to initalize things before running an update. The drawback is that computed
	// values and layouting are not performed yet, resulting in default values when
	// querying such information in the event handler.
	PluginRegistry::NotifyDocumentLoad(document);
	document->DispatchEvent(EventId::Load, Dictionary());

	// Data models are updated after the 'load' event so that the user has a chance to change
	// any data variables first. We do not clear dirty variables here, since users may need to
	// retrieve whether or not eg. a data variable has changed in a controller.
	for (auto& data_model : data_models)
		data_model.second->Update(false);

	document->UpdateDocument();

	return document;
}

ElementDocument* Context::LoadDocumentFromMemory(const String& string, const String& source_url)
{
	// Open the stream based on the string contents.
	auto stream = MakeUnique<StreamMemory>(reinterpret_cast<const byte*>(string.c_str()), string.size());

	stream->SetSourceURL(source_url);

	// Load the document from the stream.
	ElementDocument* document = LoadDocument(stream.get());

	return document;
}

void Context::UnloadDocument(ElementDocument* _document)
{
	// Has this document already been unloaded?
	for (size_t i = 0; i < unloaded_documents.size(); ++i)
	{
		if (unloaded_documents[i].get() == _document)
			return;
	}

	ElementDocument* document = _document;

	if (document->GetParentNode() == root.get())
	{
		// Dispatch the unload notifications.
		document->DispatchEvent(EventId::Unload, Dictionary());
		PluginRegistry::NotifyDocumentUnload(document);

		// Move document to a temporary location to be released later.
		unloaded_documents.push_back(root->RemoveChild(document));
	}

	// Remove the item from the focus history.
	ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (itr != document_focus_history.end())
		document_focus_history.erase(itr);

	// Focus to the previous document if the old document is the current focus.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

	// Clear the active element if the old document is the active element.
	if (active && active->GetOwnerDocument() == document)
	{
		active = nullptr;
	}

	// Clear other pointers to elements whose owner was deleted
	if (drag && drag->GetOwnerDocument() == document)
	{
		drag = nullptr;
		ReleaseDragClone();
	}

	if (drag_hover && drag_hover->GetOwnerDocument() == document)
	{
		drag_hover = nullptr;
	}

	// Rebuild the hover state.
	UpdateHoverChain(mouse_position);
}

void Context::UnloadAllDocuments()
{
	// Unload all children.
	while (root->GetNumChildren(true) > 0)
		UnloadDocument(root->GetChild(0)->GetOwnerDocument());

	// The element lists may point to elements that are getting removed.
	active_chain.clear();
	hover_chain.clear();
	drag_hover_chain.clear();
}

void Context::EnableMouseCursor(bool enable)
{
	// The cursor is set to an invalid name so that it is forced to update in the next update loop.
	cursor_name = ":reset:";
	enable_cursor = enable;
}

void Context::ActivateTheme(const String& theme_name, bool activate)
{
	bool theme_changed = false;

	if (activate)
		theme_changed = active_themes.insert(theme_name).second;
	else
		theme_changed = (active_themes.erase(theme_name) > 0);

	if (theme_changed)
	{
		for (int i = 0; i < root->GetNumChildren(true); ++i)
		{
			if (ElementDocument* document = root->GetChild(i)->GetOwnerDocument())
				document->DirtyMediaQueries();
		}
	}
}

bool Context::IsThemeActive(const String& theme_name) const
{
	return active_themes.count(theme_name);
}

ElementDocument* Context::GetDocument(const String& id)
{
	for (int i = 0; i < root->GetNumChildren(); i++)
	{
		ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
		if (document == nullptr)
			continue;

		if (document->GetId() == id)
			return document;
	}

	return nullptr;
}

ElementDocument* Context::GetDocument(int index)
{
	Element* element = root->GetChild(index);
	if (element == nullptr)
		return nullptr;

	return element->GetOwnerDocument();
}

int Context::GetNumDocuments() const
{
	return root->GetNumChildren();
}

Element* Context::GetHoverElement()
{
	return hover;
}

Element* Context::GetFocusElement()
{
	return focus;
}

Element* Context::GetRootElement()
{
	return root.get();
}

SelectionController* Context::GetSelectionController()
{
	return selection_controller.get();
}

void Context::SetTouchLongPressCallback(TouchLongPressCallback callback)
{
	touch_long_press_callback = std::move(callback);
}

void Context::SetTextLoupeRenderCallback(TextLoupeRenderCallback callback)
{
	text_loupe_render_callback = std::move(callback);
}

TextLoupeState Context::GetTextLoupeState() const
{
	TextLoupeState state;
	state.active = text_loupe_static_state.active || text_loupe_widget_active;
	if (text_loupe_widget_active)
		state.anchor = text_loupe_widget_anchor;
	else
		state.anchor = text_loupe_static_state.anchor;
	return state;
}

bool Context::HasActiveTouch() const
{
	return !touch_states.empty();
}

void Context::SetTextLoupeFromWidget(bool active, Vector2f anchor)
{
	text_loupe_widget_active = active;
	if (active)
		text_loupe_widget_anchor = anchor;
}

void Context::RefreshTextLoupeState(Vector2f anchor)
{
	text_loupe_static_state.active = false;
	if (touch_states.empty())
		return;

	if (selection_controller->IsDragging() || selection_controller->IsHandleDragging())
	{
		text_loupe_static_state.active = true;
		text_loupe_static_state.anchor = anchor;
	}
}

void Context::ClearTextLoupeState()
{
	text_loupe_static_state = {};
	text_loupe_widget_active = false;
}

bool Context::AnyTouchSelectionArmed() const
{
	for (const auto& entry : touch_states)
	{
		if (entry.second.selection_armed)
			return true;
	}
	return false;
}

void Context::UpdateTouchGestures()
{
	if (touch_states.empty())
		return;

	const double current_time = GetSystemInterface()->GetElapsedTime();
	const float slop = TOUCH_SCROLL_SLOP * density_independent_pixel_ratio;

	for (auto& entry : touch_states)
	{
		TouchState& state = entry.second;
		if (state.long_press_fired || state.selection_armed || state.touch_scrolling)
			continue;

		if (current_time - state.touch_start_time < TOUCH_LONG_PRESS_TIME)
			continue;

		const Vector2f delta = state.last_position - state.start_position;
		if (delta.SquaredMagnitude() > slop * slop)
			continue;

		state.long_press_fired = true;
		Element* target = state.touch_target ? state.touch_target : hover;
		if (!target)
			continue;

		const Vector2i position(static_cast<int>(state.start_position.x), static_cast<int>(state.start_position.y));
		if (IsTextEditorTarget(target))
		{
			if (touch_long_press_callback)
				touch_long_press_callback(position, target);
			continue;
		}

		if (selection_controller->CanSelectStaticText(target))
		{
			selection_controller->SelectWordAt(position);
			state.selection_armed = true;
		}

		if (touch_long_press_callback)
			touch_long_press_callback(position, target);
	}
}

void Context::PullDocumentToFront(ElementDocument* document)
{
	if (document != root->GetLastChild())
	{
		// Calling RemoveChild() / AppendChild() would be cleaner, but that dirties the document's layout
		// unnecessarily, so we'll go under the hood here.
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin() + root->GetNumChildren(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::PushDocumentToBack(ElementDocument* document)
{
	if (document != root->GetFirstChild())
	{
		// See PullDocumentToFront().
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

void Context::UnfocusDocument(ElementDocument* document)
{
	auto it = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (it != document_focus_history.end())
		document_focus_history.erase(it);

	if (!document_focus_history.empty())
		document_focus_history.back()->GetFocusLeafNode()->Focus();
}

void Context::AddEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->AddEventListener(event, listener, in_capture_phase);
}

void Context::RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->RemoveEventListener(event, listener, in_capture_phase);
}

bool Context::ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	if (selection_controller->OnKeyDown(key_identifier, key_modifier_state))
		return true;

	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keydown, parameters);
	else
		return root->DispatchEvent(EventId::Keydown, parameters);
}

bool Context::ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keyup, parameters);
	else
		return root->DispatchEvent(EventId::Keyup, parameters);
}

bool Context::ProcessTextInput(char character)
{
	// Only the standard ASCII character set is a valid subset of UTF-8.
	if (static_cast<unsigned char>(character) > 127)
		return false;
	return ProcessTextInput(static_cast<Character>(character));
}

bool Context::ProcessTextInput(Character character)
{
	// Generate the parameters for the key event.
	String text = StringUtilities::ToUTF8(character);
	return ProcessTextInput(text);
}

bool Context::ProcessTextInput(const String& string)
{
	Element* target = (focus ? focus : root.get());

	Dictionary parameters;
	parameters["text"] = string;

	bool consumed = target->DispatchEvent(EventId::Textinput, parameters);

	return consumed;
}

bool Context::ProcessMouseMove(int x, int y, int key_modifier_state)
{
	// Check whether the mouse moved since the last event came through.
	Vector2i old_mouse_position = mouse_position;
	mouse_position = {x, y};
	const bool mouse_moved = (mouse_position != old_mouse_position || !mouse_active);
	mouse_active = true;

	// Update the current hover chain. This will send all necessary 'onmouseout', 'onmouseover', 'ondragout' and 'ondragover' messages.
	Dictionary parameters, drag_parameters;
	UpdateHoverChain(old_mouse_position, key_modifier_state, &parameters, &drag_parameters);

	// Dispatch any 'onmousemove' events.
	if (mouse_moved)
	{
		if (hover)
		{
			hover->DispatchEvent(EventId::Mousemove, parameters);

			if (drag_hover && drag_verbose)
				drag_hover->DispatchEvent(EventId::Dragmove, drag_parameters);
		}
	}

	if (touch_states.empty() || AnyTouchSelectionArmed())
	{
		if (selection_controller->IsHandleDragging())
			selection_controller->UpdateHandleDrag(mouse_position);
		else
			selection_controller->OnPointerMove(mouse_position);
	}

	return !IsMouseInteracting();
}

static Element* FindFocusElement(Element* element)
{
	ElementDocument* owner_document = element->GetOwnerDocument();
	if (!owner_document || owner_document->GetComputedValues().focus() == Style::Focus::None)
		return nullptr;

	while (element && element->GetComputedValues().focus() == Style::Focus::None)
	{
		element = element->GetParentNode();
	}

	return element;
}

bool Context::ProcessMouseButtonDown(int button_index, int key_modifier_state)
{
	mouse_active = true;

	// Refresh hover before focus/selection handling so clicks after layout changes hit the right target.
	UpdateHoverChain(mouse_position, key_modifier_state);

	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	bool propagate = true;

	if (button_index == 0)
	{
		if (selection_controller->HasSelection())
		{
			const SelectionHandleSide handle = selection_controller->HitTestHandle(mouse_position);
			if (handle != SelectionHandleSide::None)
			{
				selection_controller->BeginHandleDrag(handle);
				active = hover;
				active_chain.clear();
				active_chain.insert(active_chain.end(), hover_chain.begin(), hover_chain.end());
				return !IsMouseInteracting();
			}
		}

		if (!IsContextMenuTarget(hover))
			selection_controller->ClearUnlessHover(hover);

		Element* interactive = ClickRouting::FindInteractiveElement(hover);

		// Set the currently hovered element to focus if it isn't already the focus.
		Element* new_focus = nullptr;
		if (hover)
		{
			new_focus = FindFocusElement(hover);
			if (new_focus && new_focus != focus && new_focus->GetComputedValues().focus() != Style::Focus::None)
				new_focus->Focus();
		}

		// Deepest element under the pointer at press time (browser-style press target).
		active = hover;

		if (hover && IsTextEditorTarget(hover))
		{
			selection_controller->ClearSelection();
			selection_controller->OnPointerUp();
		}

		const bool touch_pointer = !touch_states.empty();
		const bool defer_static_selection = touch_pointer && hover && selection_controller->CanSelectStaticText(hover);

		float mouse_distance_squared = float((mouse_position - last_click_mouse_position).SquaredMagnitude());
		float max_mouse_distance = DOUBLE_CLICK_MAX_DIST * density_independent_pixel_ratio;
		double click_time = GetSystemInterface()->GetElapsedTime();
		Element* click_identity = interactive ? interactive : active;

		int click_count = 1;
		if (click_identity && click_identity == last_click_element && float(click_time - last_click_time) < DOUBLE_CLICK_TIME &&
			mouse_distance_squared < max_mouse_distance * max_mouse_distance)
		{
			click_count = last_click_count + 1;
		}
		parameters["click_count"] = click_count;

		if (hover && !defer_static_selection)
			selection_controller->OnPointerDown(hover, mouse_position);

		// Call 'onmousedown' on every item in the hover chain, and copy the hover chain to the active chain.
		if (hover)
			propagate = hover->DispatchEvent(EventId::Mousedown, parameters);

		if (propagate && click_count == 2 && hover)
			propagate = hover->DispatchEvent(EventId::Dblclick, parameters);

		if (touch_pointer && hover && click_count == 2 && selection_controller->CanSelectStaticText(hover))
		{
			selection_controller->SelectWordAt(mouse_position);
			for (auto& entry : touch_states)
				entry.second.selection_armed = true;
		}

		if (click_count >= 3)
		{
			last_click_element = nullptr;
			last_click_time = 0;
			last_click_count = 0;
		}
		else
		{
			last_click_element = click_identity;
			last_click_time = click_time;
			last_click_count = click_count;
		}

		last_click_mouse_position = mouse_position;

		active_chain.clear();
		active_chain.insert(active_chain.end(), hover_chain.begin(), hover_chain.end());

		if (propagate && !selection_controller->IsDragging())
		{
			// Traverse down the hierarchy of the newly focused element (if any), and see if we can begin dragging it.
			drag_started = false;
			drag = hover;
			while (drag)
			{
				Style::Drag drag_style = drag->GetComputedValues().drag();
				switch (drag_style)
				{
				case Style::Drag::None: drag = drag->GetParentNode(); continue;
				case Style::Drag::Block: drag = nullptr; continue;
				default: drag_verbose = (drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone);
				}

				break;
			}
		}
		else if (selection_controller->IsDragging())
		{
			drag_started = false;
			drag = nullptr;
		}

#ifdef RMLUI_DEBUG
		LogPointerInteraction("mousedown", hover, interactive, active, false, false);
#endif
	}
	else
	{
		// Not the primary mouse button, so we're not doing any special processing.
		if (hover)
			propagate = hover->DispatchEvent(EventId::Mousedown, parameters);
	}

	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
	}
	else if (button_index == 2 && hover && propagate)
	{
		Dictionary scroll_parameters;
		GenerateMouseEventParameters(scroll_parameters);
		GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
		scroll_parameters["autoscroll"] = true;

		// Dispatch a mouse scroll event, this gives elements an opportunity to block autoscroll from being initialized.
		if (hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
			scroll_controller->ActivateAutoscroll(hover->GetClosestScrollableContainer(), mouse_position);
	}

	return !IsMouseInteracting();
}

bool Context::ProcessMouseButtonUp(int button_index, int key_modifier_state)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	// We want to return the interaction state before handling the mouse up events, so that any active element that is released is considered to
	// capture the event.
	const bool result = !IsMouseInteracting();

	// Process primary click.
	if (button_index == 0)
	{
		UpdateHoverChain(mouse_position, key_modifier_state);

		// The elements in the new hover chain have the 'onmouseup' event called on them.
		if (hover)
			hover->DispatchEvent(EventId::Mouseup, parameters);

		selection_controller->EndHandleDrag();
		selection_controller->OnPointerUp();

		// Click the deepest compatible press/release target. Interactive controls (buttons, data-event-click)
		// still activate via ClickRouting when geometry drifts between mousedown and mouseup.
		const Vector2f mouse_point(float(mouse_position.x), float(mouse_position.y));
		Element* press_hover = active;
		Element* click_target = ClickRouting::ResolveClickTarget(press_hover, hover, mouse_point, FindFocusElement);

		// Reset before dispatching click. Handlers may rebuild the DOM (e.g. chat message list), destroying
		// elements still referenced in active_chain; iterating them after dispatch is use-after-free.
		ResetActiveChain();

		const bool click_dispatched = click_target && click_target->GetOwnerDocument();
		if (click_dispatched)
			click_target->DispatchEvent(EventId::Click, parameters);

#ifdef RMLUI_DEBUG
		LogPointerInteraction("mouseup", hover, ClickRouting::FindInteractiveElement(hover), press_hover, false, click_dispatched);
#endif

		// Click handlers may rebuild the DOM; refresh hover/cursor for the current mouse position.
		ProcessMouseMove(mouse_position.x, mouse_position.y, key_modifier_state);

		if (drag)
		{
			if (drag_started)
			{
				Dictionary drag_parameters;
				GenerateMouseEventParameters(drag_parameters);
				GenerateDragEventParameters(drag_parameters);
				GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

				if (drag_hover)
				{
					if (drag_verbose)
					{
						drag_hover->DispatchEvent(EventId::Dragdrop, drag_parameters);
						// User may have removed the element, do an extra check.
						if (drag_hover)
							drag_hover->DispatchEvent(EventId::Dragout, drag_parameters);
					}
				}

				if (drag)
					drag->DispatchEvent(EventId::Dragend, drag_parameters);

				ReleaseDragClone();
			}

			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();

			// We may have changes under our mouse, this ensures that the hover chain is properly updated
			ProcessMouseMove(mouse_position.x, mouse_position.y, key_modifier_state);
		}
	}
	else
	{
		// Not the left mouse button, so we're not doing any special processing.
		if (hover)
			hover->DispatchEvent(EventId::Mouseup, parameters);
	}

	// If we have autoscrolled while holding the middle mouse button, release the autoscroll mode now.
	if (scroll_controller->HasAutoscrollMoved())
		scroll_controller->Reset();

	return result;
}

bool Context::ProcessMouseWheel(float wheel_delta, int key_modifier_state)
{
	return ProcessMouseWheel(Vector2f{0.f, wheel_delta}, key_modifier_state);
}

bool Context::ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state)
{
	if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
	{
		scroll_controller->Reset();
		return false;
	}
	else if (!hover)
	{
		scroll_controller->Reset();
		return true;
	}

	Dictionary scroll_parameters;
	GenerateMouseEventParameters(scroll_parameters);
	GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
	scroll_parameters["wheel_delta_x"] = wheel_delta.x;
	scroll_parameters["wheel_delta_y"] = wheel_delta.y;

	// Dispatch a mouse scroll event, this gives elements an opportunity to block scrolling from being performed.
	if (!hover->DispatchEvent(EventId::Mousescroll, scroll_parameters))
		return false;

	const float unit_scroll_length = UNIT_SCROLL_LENGTH * density_independent_pixel_ratio;
	const Vector2f scroll_length = wheel_delta * unit_scroll_length;
	Element* target = hover->GetClosestScrollableContainer();

	if (scroll_controller->GetMode() == ScrollController::Mode::Smoothscroll && scroll_controller->GetTarget() == target)
		scroll_controller->IncrementSmoothscrollTarget(scroll_length);
	else
		scroll_controller->ActivateSmoothscroll(target, scroll_length, ScrollBehavior::Auto);

	return target == nullptr;
}

bool Context::ProcessMouseLeave()
{
	mouse_active = false;

	ResetActiveChain();

	// Update the hover chain. Now that 'mouse_active' is disabled this will remove the hover state from all elements.
	UpdateHoverChain(mouse_position);

	return !IsMouseInteracting();
}

bool Context::IsMouseInteracting() const
{
	return (hover && hover != root.get()) || (active && active != root.get()) || scroll_controller->GetMode() == ScrollController::Mode::Autoscroll;
}

Context::TouchState* Context::LookupTouch(TouchId identifier)
{
	auto touch_it = touch_states.find(identifier);
	return touch_it != touch_states.end() ? &touch_it->second : nullptr;
}

bool Context::ProcessTouchStart(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchStart(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchMove(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchMove(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchEnd(const TouchList& touches, int key_modifier_state)
{
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchEnd(touch, key_modifier_state);
	return result;
}

bool Context::ProcessTouchCancel(const TouchList& touches)
{
	bool result = true;
	for (const auto& touch : touches)
		result &= ProcessTouchCancel(touch);
	return result;
}

bool Context::ProcessTouchStart(const Touch& touch, int key_modifier_state)
{
	TouchState* state = LookupTouch(touch.identifier);
	RMLUI_ASSERTMSG(state == nullptr, "Receiving touch start event for an already started touch.");
	if (!state)
	{
		auto it_inserted = touch_states.emplace(touch.identifier, TouchState()).first;
		state = &it_inserted->second;
	}

	state->start_position = touch.position;
	state->last_position = touch.position;
	state->scrolling_last_time = GetSystemInterface()->GetElapsedTime();
	state->touch_scrolling = false;
	state->selection_armed = false;
	state->long_press_fired = false;
	state->touch_start_time = state->scrolling_last_time;
	state->ClearSamples();
	state->PushSample(touch.position, state->scrolling_last_time);

	Element* touch_element = GetElementAtPoint(touch.position);
	state->touch_target = touch_element;
	state->scroll_container = touch_element ? touch_element->GetClosestScrollableContainer() : nullptr;

	// Interrupt any coast / rubber-band settle when a new touch begins.
	if (scroll_controller->GetMode() != ScrollController::Mode::None || scroll_controller->HasVisualOverscroll())
		scroll_controller->Reset();

	ProcessMouseMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);

	if (selection_controller->HasSelection())
	{
		const Vector2i touch_position(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y));
		const SelectionHandleSide handle = selection_controller->HitTestHandle(touch_position);
		if (handle != SelectionHandleSide::None)
		{
			selection_controller->BeginHandleDrag(handle);
			state->selection_armed = true;
			return true;
		}
	}

	// always assume touch press/release events are handled as left mouse button
	return ProcessMouseButtonDown(0, key_modifier_state);
}

bool Context::ProcessTouchMove(const Touch& touch, int key_modifier_state)
{
	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return true;

	state->scrolling_last_time = GetSystemInterface()->GetElapsedTime();

	const float scroll_slop = TOUCH_SCROLL_SLOP * density_independent_pixel_ratio;
	const Vector2f delta_from_start = touch.position - state->start_position;
	if (!state->selection_armed && (Math::Absolute(delta_from_start.y) > scroll_slop &&
			Math::Absolute(delta_from_start.y) >= Math::Absolute(delta_from_start.x)))
		state->touch_scrolling = true;

	if (state->scroll_container)
	{
		const Vector2f delta = touch.position - state->last_position;

		if (drag || (selection_controller->IsDragging() && state->selection_armed) || selection_controller->IsHandleDragging())
		{
			// Don't scroll and reset scrolling state when dragging any element (scrollbars and others)
			// or drag-selecting static text inside a scroll container.
			state->last_position = touch.position;
			state->ClearSamples();
			state->PushSample(touch.position, state->scrolling_last_time);
		}
		else if (delta.x != 0 || delta.y != 0)
		{
			// Finger-tracking with rubber-band overscroll past edges.
			scroll_controller->InstantScrollOnTarget(state->scroll_container, -delta, true);
			state->PushSample(touch.position, state->scrolling_last_time);

			const float touch_max_distance = TOUCH_CLICK_MAX_DISTANCE * density_independent_pixel_ratio;
			if (delta_from_start.SquaredMagnitude() >= touch_max_distance * touch_max_distance)
				ResetActiveChain();
		}
	}

	state->last_position = touch.position;

	RefreshTextLoupeState(touch.position);

	return ProcessMouseMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);
}

bool Context::ProcessTouchEnd(const Touch& touch, int key_modifier_state)
{
	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return true;

	if (!state->selection_armed && !state->long_press_fired && !state->touch_scrolling)
		selection_controller->OnTouchTap(Vector2i(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y)), hover);

	if (state->scroll_container)
	{
		const double current_time = GetSystemInterface()->GetElapsedTime();
		const double time_since_last_move = current_time - state->scrolling_last_time;

		Vector2f velocity;
		if (time_since_last_move < SCROLL_INERTIA_DELAY && state->sample_count >= 2)
		{
			constexpr int capacity = TouchState::VelocitySampleCapacity;
			const auto& newest = state->samples[(state->sample_write + capacity - 1) % capacity];
			const TouchState::Sample* oldest_in_window = &newest;
			for (int i = 1; i < state->sample_count; i++)
			{
				const auto& candidate = state->samples[(state->sample_write + capacity - 1 - i) % capacity];
				if (newest.time - candidate.time > TOUCH_VELOCITY_WINDOW)
					break;
				oldest_in_window = &candidate;
			}

			const float dt = static_cast<float>(newest.time - oldest_in_window->time);
			if (dt > 1e-4f)
				velocity = (oldest_in_window->position - newest.position) / dt;
		}

		const float scroll_top = state->scroll_container->GetScrollTop();
		const float scroll_left = state->scroll_container->GetScrollLeft();
		const float max_top = Math::Max(0.f, state->scroll_container->GetScrollHeight() - state->scroll_container->GetClientHeight());
		const float max_left = Math::Max(0.f, state->scroll_container->GetScrollWidth() - state->scroll_container->GetClientWidth());
		constexpr float overscroll_eps = 0.5f;
		const bool overscrolled = scroll_top < -overscroll_eps || scroll_left < -overscroll_eps || scroll_top > max_top + overscroll_eps ||
			scroll_left > max_left + overscroll_eps;

		if (overscrolled)
			scroll_controller->ActivateOverscrollSettle(state->scroll_container, velocity);
		else if (velocity.x != 0.f || velocity.y != 0.f)
			scroll_controller->ActivateInertia(state->scroll_container, velocity);
	}

	touch_states.erase(touch.identifier);

	ClearTextLoupeState();

	ProcessMouseMove(static_cast<int>(touch.position.x), static_cast<int>(touch.position.y), key_modifier_state);

	// always assume touch press/release events are handled as left mouse button
	const bool result = ProcessMouseButtonUp(0, key_modifier_state);
	// Clear sticky :hover after the last finger lifts so remount/layout shifts
	// (e.g. compact nav pill reflow) cannot leave hover fill on the wrong control.
	if (touch_states.empty())
		ProcessMouseLeave();
	return result;
}

bool Context::ProcessTouchCancel(const Touch& touch)
{
	TouchState* state = LookupTouch(touch.identifier);
	if (!state)
		return false;

	touch_states.erase(touch.identifier);

	ClearTextLoupeState();

	const bool result = ProcessMouseButtonUp(0, 0);
	if (touch_states.empty())
		ProcessMouseLeave();
	return result;
}

void Context::SetDefaultScrollBehavior(ScrollBehavior scroll_behavior, float speed_factor)
{
	scroll_controller->SetDefaultScrollBehavior(scroll_behavior, speed_factor);
}

void Context::SetScrollOverscrollEdges(bool min_x, bool max_x, bool min_y, bool max_y)
{
	scroll_controller->SetOverscrollEdgesEnabled(min_x, max_x, min_y, max_y);
}

void Context::ClearScrollOverscroll()
{
	scroll_controller->ClearPendingOverscroll();
}

RenderManager& Context::GetRenderManager()
{
	return *render_manager;
}

TextInputHandler* Context::GetTextInputHandler() const
{
	return text_input_handler;
}

void Context::SetInstancer(ContextInstancer* _instancer)
{
	RMLUI_ASSERT(instancer == nullptr);
	instancer = _instancer;
}

DataModelConstructor Context::CreateDataModel(const String& name, DataTypeRegister* data_type_register)
{
	if (!data_type_register)
	{
		if (!default_data_type_register)
			default_data_type_register = MakeUnique<DataTypeRegister>();
		data_type_register = default_data_type_register.get();
	}

	auto result = data_models.emplace(name, MakeUnique<DataModel>(data_type_register));
	bool inserted = result.second;
	if (inserted)
		return DataModelConstructor(result.first->second.get());

	Log::Message(Log::LT_ERROR, "Data model name '%s' already exists.", name.c_str());
	return DataModelConstructor();
}

DataModelConstructor Context::GetDataModel(const String& name)
{
	if (DataModel* model = GetDataModelPtr(name))
		return DataModelConstructor(model);

	Log::Message(Log::LT_ERROR, "Data model name '%s' could not be found.", name.c_str());
	return DataModelConstructor();
}

UnorderedMap<String, DataModelConstructor> Context::GetDataModels() const
{
	UnorderedMap<String, DataModelConstructor> result;
	result.reserve(data_models.size());
	for (const auto& pair : data_models)
		result.emplace(pair.first, DataModelConstructor(pair.second.get()));
	return result;
}

bool Context::RemoveDataModel(const String& name)
{
	auto it = data_models.find(name);
	if (it == data_models.end())
		return false;

	DataModel* model = it->second.get();
	ElementList elements = model->GetAttachedModelRootElements();

	for (Element* element : elements)
		element->SetDataModel(nullptr);

	data_models.erase(it);

	return true;
}

void Context::OnElementDetach(Element* element)
{
	auto it_hover = hover_chain.find(element);
	if (it_hover != hover_chain.end())
	{
		Dictionary parameters;
		GenerateMouseEventParameters(parameters, -1);
		element->DispatchEvent(EventId::Mouseout, parameters);

		hover_chain.erase(it_hover);

		if (hover == element)
			hover = nullptr;
	}

	auto it_active = std::find(active_chain.begin(), active_chain.end(), element);
	if (it_active != active_chain.end())
	{
		active_chain.erase(it_active);
	}

	// Clear active even when the chain has not been copied yet (e.g. element destroyed
	// mid-mousedown before active_chain is populated from hover_chain).
	if (active == element)
		active = nullptr;

	if (last_click_element == element)
		last_click_element = nullptr;

	if (drag)
	{
		auto it = drag_hover_chain.find(element);
		if (it != drag_hover_chain.end())
		{
			drag_hover_chain.erase(it);

			if (drag_hover == element)
				drag_hover = nullptr;
		}

		if (drag == element)
		{
			// The dragged element is being removed, silently cancel the drag operation
			if (drag_started)
				ReleaseDragClone();

			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();
		}
	}

	// Focus normally cleared and set by parent during Element::RemoveChild.
	// However, there are some exceptions, such as when an there are multiple
	// ElementDocuments in the hierarchy above the current element.
	if (element == focus)
		focus = nullptr;

	// If the element is a document lower down in the hierarchy, we may need to remove it from the focus history.
	if (element->GetOwnerDocument() == element)
	{
		auto it = std::find(document_focus_history.begin(), document_focus_history.end(), element);
		if (it != document_focus_history.end())
			document_focus_history.erase(it);
	}

	if (scroll_controller->GetTarget() == element)
		scroll_controller->Reset();

	// Clear TouchState if we're touching element
	for (auto touch_it = touch_states.begin(); touch_it != touch_states.end();)
	{
		if (touch_it->second.scroll_container == element)
			touch_it = touch_states.erase(touch_it);
		else
			++touch_it;
	}
}

bool Context::OnFocusChange(Element* new_focus, bool focus_visible)
{
	RMLUI_ASSERT(new_focus);

	ElementSet old_chain;
	ElementSet new_chain;

	Element* old_focus = focus;
	ElementDocument* old_document = old_focus ? old_focus->GetOwnerDocument() : nullptr;
	ElementDocument* new_document = new_focus->GetOwnerDocument();

	// If the current focus is modal and the new focus is cannot receive focus from modal, deny the request.
	if (old_document && old_document->IsModal() && (!new_document || !(new_document->IsModal() || new_document->IsFocusableFromModal())))
		return false;

	// If the document of the new focus has been closed, deny the request.
	if (std::find_if(unloaded_documents.begin(), unloaded_documents.end(),
			[&](const auto& unloaded_document) { return unloaded_document.get() == new_document; }) != unloaded_documents.end())
	{
		return false;
	}

	// Build the old chains
	Element* element = old_focus;
	while (element)
	{
		old_chain.insert(element);
		element = element->GetParentNode();
	}

	// Build the new chain
	element = new_focus;
	while (element)
	{
		new_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send out blur/focus events.
	Dictionary parameters;
	SendEvents(old_chain, new_chain, EventId::Blur, parameters);

	if (focus_visible)
		parameters["focus_visible"] = true;

	SendEvents(new_chain, old_chain, EventId::Focus, parameters);

	focus = new_focus;

	// Raise the element's document to the front, if desired.
	ElementDocument* document = focus->GetOwnerDocument();
	if (document != nullptr)
	{
		Style::ZIndex z_index_property = document->GetComputedValues().z_index();
		if (z_index_property.type == Style::ZIndex::Auto)
			document->PullToFront();
	}

	// Update the focus history
	if (old_document != new_document)
	{
		// If documents have changed, add the new document to the end of the history
		ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), new_document);
		if (itr != document_focus_history.end())
			document_focus_history.erase(itr);

		if (new_document != nullptr)
			document_focus_history.push_back(new_document);
	}

	return true;
}

void Context::GenerateClickEvent(Element* element)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, 0);

	element->DispatchEvent(EventId::Click, parameters);
}

void Context::UpdateHoverChain(Vector2i old_mouse_position, int key_modifier_state, Dictionary* out_parameters, Dictionary* out_drag_parameters)
{
	const Vector2f position(mouse_position);

	Dictionary local_parameters, local_drag_parameters;
	Dictionary& parameters = out_parameters ? *out_parameters : local_parameters;
	Dictionary& drag_parameters = out_drag_parameters ? *out_drag_parameters : local_drag_parameters;

	// Generate the parameters for the mouse events (there could be a few!).
	GenerateMouseEventParameters(parameters);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	GenerateMouseEventParameters(drag_parameters);
	GenerateDragEventParameters(drag_parameters);
	GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

	// Send out drag events.
	if (drag && !selection_controller->IsDragging())
	{
		if (mouse_position != old_mouse_position)
		{
			if (!drag_started)
			{
				Dictionary drag_start_parameters = drag_parameters;
				drag_start_parameters["mouse_x"] = old_mouse_position.x;
				drag_start_parameters["mouse_y"] = old_mouse_position.y;
				drag->DispatchEvent(EventId::Dragstart, drag_start_parameters);
				drag_started = true;

				if (drag->GetComputedValues().drag() == Style::Drag::Clone)
				{
					// Clone the element and attach it to the mouse cursor.
					CreateDragClone(drag);
				}
			}

			drag->DispatchEvent(EventId::Drag, drag_parameters);
		}
	}

	hover = mouse_active ? GetElementAtPoint(position) : nullptr;
	hover = PreferContentOverScrollbar(this, position, hover);

	if (enable_cursor)
	{
		String new_cursor_name;

		if (scroll_controller->GetMode() == ScrollController::Mode::Autoscroll)
			new_cursor_name = scroll_controller->GetAutoscrollCursor(mouse_position, density_independent_pixel_ratio);
		else if (drag)
			new_cursor_name = drag->GetComputedValues().cursor();
		else if (hover)
			new_cursor_name = GetEffectiveCursor(hover);

		if (new_cursor_name != cursor_name)
		{
			GetSystemInterface()->SetMouseCursor(new_cursor_name);
			cursor_name = new_cursor_name;
		}
	}

	// Build the new hover chain.
	ElementSet new_hover_chain;
	Element* element = hover;
	while (element != nullptr)
	{
		new_hover_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send mouseout / mouseover events.
	SendEvents(hover_chain, new_hover_chain, EventId::Mouseout, parameters);
	SendEvents(new_hover_chain, hover_chain, EventId::Mouseover, parameters);

	// Send out drag events.
	if (drag && mouse_active)
	{
		drag_hover = GetElementAtPoint(position, drag);

		ElementSet new_drag_hover_chain;
		element = drag_hover;
		while (element != nullptr)
		{
			new_drag_hover_chain.insert(element);
			element = element->GetParentNode();
		}

		if (drag_started && drag_verbose)
		{
			// Send out ondragover and ondragout events as appropriate.
			SendEvents(drag_hover_chain, new_drag_hover_chain, EventId::Dragout, drag_parameters);
			SendEvents(new_drag_hover_chain, drag_hover_chain, EventId::Dragover, drag_parameters);
		}

		drag_hover_chain.swap(new_drag_hover_chain);
	}

	// Swap the new chain in.
	hover_chain.swap(new_hover_chain);
}

void Context::ResetActiveChain()
{
	// Unset the 'active' pseudo-class on all the elements in the active chain; because they may not necessarily
	// have had 'onmouseup' called on them, we can't guarantee this has happened already.
	for (Element* element : active_chain)
		element->SetPseudoClass("active", false);

	active_chain.clear();
	active = nullptr;
}

Element* Context::GetElementAtPoint(Vector2f point, const Element* ignore_element, Element* element) const
{
	if (!element)
	{
		if (ignore_element == root.get())
			return nullptr;

		element = root.get();
	}

	bool is_modal = false;
	ElementDocument* focus_document = nullptr;

	// If we have modal focus, only check down documents that can receive focus from modals.
	if (element == root.get() && focus)
	{
		focus_document = focus->GetOwnerDocument();
		if (focus_document && focus_document->IsModal())
			is_modal = true;
	}

	// Check any elements within our stacking context. We want to return the lowest-down element
	// that is under the cursor.
	if (element->local_stacking_context)
	{
		if (element->stacking_context_dirty)
			element->BuildLocalStackingContext();

		for (int i = (int)element->stacking_context.size() - 1; i >= 0; --i)
		{
			Element* stacking_child = element->stacking_context[i];
			if (ignore_element)
			{
				// Check if the element is a descendant of the element we're ignoring.
				Element* element_hierarchy = stacking_child;
				while (element_hierarchy)
				{
					if (element_hierarchy == ignore_element)
						break;

					element_hierarchy = element_hierarchy->GetParentNode();
				}

				if (element_hierarchy)
					continue;
			}

			if (is_modal)
			{
				ElementDocument* child_document = stacking_child->GetOwnerDocument();
				if (!child_document || !(child_document == focus_document || child_document->IsFocusableFromModal()))
					continue;
			}

			Element* child_element = GetElementAtPoint(point, ignore_element, stacking_child);
			if (child_element)
				return child_element;
		}
	}

	// Ignore elements whose pointer events are disabled.
	if (element->GetComputedValues().pointer_events() == Style::PointerEvents::None)
		return nullptr;

	// Projection may fail if we have a singular transformation matrix.
	bool projection_result = element->Project(point);

	// Check if the point is actually within this element.
	bool within_element = (projection_result && element->IsPointWithinElement(point));
	if (within_element)
	{
		// The element may have been clipped out of view if it overflows an ancestor, so check its clipping region.
		Rectanglei clip_region;
		if (ElementUtilities::GetClippingRegion(element, clip_region))
			within_element = clip_region.Contains(Vector2i(point));
	}

	if (within_element && element->IsVisible())
		return element;

	return nullptr;
}

void Context::CreateDragClone(Element* element)
{
	RMLUI_ASSERTMSG(cursor_proxy, "Unable to create drag clone, no cursor proxy document.");

	ReleaseDragClone();

	// Instance the drag clone.
	ElementPtr element_drag_clone = element->Clone();
	if (!element_drag_clone)
	{
		Log::Message(Log::LT_ERROR, "Unable to duplicate drag clone.");
		return;
	}

	// Set the style sheet on the cursor proxy.
	if (ElementDocument* document = element->GetOwnerDocument())
	{
		// Borrow the target document's style sheet. Sharing style sheet containers should be used with care, and
		// only within the same context.
		static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheetContainer(document->style_sheet_container);
	}

	drag_clone = element_drag_clone.get();

	// Append the clone to the cursor proxy element.
	cursor_proxy->AppendChild(std::move(element_drag_clone));

	// Position the clone. Use projected mouse coordinates to handle any ancestor transforms.
	const Vector2f absolute_pos = element->GetAbsoluteOffset(BoxArea::Border);
	Vector2f projected_mouse_position = Vector2f(mouse_position);
	if (Element* parent = element->GetParentNode())
		parent->Project(projected_mouse_position);

	drag_clone->SetProperty(PropertyId::Position, Property(Style::Position::Absolute));
	drag_clone->SetProperty(PropertyId::Left, Property(absolute_pos.x - projected_mouse_position.x, Unit::PX));
	drag_clone->SetProperty(PropertyId::Top, Property(absolute_pos.y - projected_mouse_position.y, Unit::PX));
	// We remove margins so that percentage- and auto-margins are evaluated correctly.
	drag_clone->SetProperty(PropertyId::MarginLeft, Property(0.f, Unit::PX));
	drag_clone->SetProperty(PropertyId::MarginTop, Property(0.f, Unit::PX));
	drag_clone->SetPseudoClass("drag", true);
}

void Context::ReleaseDragClone()
{
	if (drag_clone)
	{
		cursor_proxy->RemoveChild(drag_clone);
		drag_clone = nullptr;
		static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheetContainer(nullptr);
	}
}

void Context::PerformSmoothscrollOnTarget(Element* target, Vector2f delta_offset, ScrollBehavior scroll_behavior)
{
	scroll_controller->ActivateSmoothscroll(target, delta_offset, scroll_behavior);
}

DataModel* Context::GetDataModelPtr(const String& name) const
{
	auto it = data_models.find(name);
	if (it != data_models.end())
		return it->second.get();
	return nullptr;
}

void Context::GenerateKeyEventParameters(Dictionary& parameters, Input::KeyIdentifier key_identifier)
{
	parameters["key_identifier"] = (int)key_identifier;
}

void Context::GenerateMouseEventParameters(Dictionary& parameters, int button_index)
{
	parameters.reserve(3);
	parameters["mouse_x"] = mouse_position.x;
	parameters["mouse_y"] = mouse_position.y;
	if (button_index >= 0)
		parameters["button"] = button_index;
}

void Context::GenerateKeyModifierEventParameters(Dictionary& parameters, int key_modifier_state)
{
	static const String property_names[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};

	for (int i = 0; i < 7; i++)
		parameters[property_names[i]] = (int)((key_modifier_state & (1 << i)) > 0);
}

void Context::GenerateDragEventParameters(Dictionary& parameters)
{
	parameters["drag_element"] = (void*)drag;
}

void Context::ReleaseUnloadedDocuments()
{
	if (!unloaded_documents.empty())
	{
		OwnedElementList documents = std::move(unloaded_documents);
		unloaded_documents.clear();

		// Clear the deleted list.
		for (size_t i = 0; i < documents.size(); ++i)
			documents[i]->GetEventDispatcher()->DetachAllEvents();
		documents.clear();
	}
}

using ElementObserverList = Vector<ObserverPtr<Element>>;

class ElementObserverListBackInserter {
public:
	using iterator_category = std::output_iterator_tag;
	using value_type = void;
	using difference_type = void;
	using pointer = void;
	using reference = void;
	using container_type = ElementObserverList;

	ElementObserverListBackInserter(ElementObserverList& elements) : elements(&elements) {}
	ElementObserverListBackInserter& operator=(Element* element)
	{
		elements->push_back(element->GetObserverPtr());
		return *this;
	}
	ElementObserverListBackInserter& operator*() { return *this; }
	ElementObserverListBackInserter& operator++() { return *this; }
	ElementObserverListBackInserter& operator++(int) { return *this; }

private:
	ElementObserverList* elements;
};

void Context::SendEvents(const ElementSet& old_items, const ElementSet& new_items, EventId id, const Dictionary& parameters)
{
	// We put our elements in observer pointers in case some of them are deleted during dispatch.
	ElementObserverList elements;
	std::set_difference(old_items.begin(), old_items.end(), new_items.begin(), new_items.end(), ElementObserverListBackInserter(elements));
	for (auto& element : elements)
	{
		if (element)
			element->DispatchEvent(id, parameters);
	}
}

void Context::Release()
{
	if (instancer)
	{
		instancer->ReleaseContext(this);
	}
}

void Context::SetDocumentsBaseTag(const String& tag)
{
	documents_base_tag = tag;
}

const String& Context::GetDocumentsBaseTag()
{
	return documents_base_tag;
}

void Context::RequestNextUpdate(double delay)
{
	RMLUI_ASSERT(delay >= 0.0);
	next_update_timeout = Math::Min(next_update_timeout, delay);
}

double Context::GetNextUpdateDelay() const
{
	return next_update_timeout;
}

} // namespace Rml
