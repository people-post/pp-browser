#include "RmlUi_Backend.h"
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "TextLoupeRenderer.h"
#include "TouchSimOverlay.h"
#include "GlBackend.h"
#include "MobileGlLifecycle.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Profiling.h>

#include <atomic>
#include <cstdio>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if SDL_MAJOR_VERSION >= 3
	#include <SDL3_image/SDL_image.h>
	#include <SDL3/SDL.h>
#else
	#include <SDL_image.h>
#endif

#if defined RMLUI_PLATFORM_EMSCRIPTEN
	#include <emscripten.h>
#elif SDL_MAJOR_VERSION == 2 && !(SDL_VIDEO_RENDER_OGL)
	#error "Only the OpenGL SDL backend is supported."
#endif

/**
    Custom render interface example for the SDL/GL3 backend.

    Overloads the OpenGL3 render interface to load textures through SDL_image's built-in texture loading functionality.
 */
class RenderInterface_GL3_SDL : public RenderInterface_GL3 {
public:
	RenderInterface_GL3_SDL() {}

	Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override
	{
		Rml::FileInterface* file_interface = Rml::GetFileInterface();
		Rml::FileHandle file_handle = file_interface->Open(source);
		if (!file_handle)
			return {};

		file_interface->Seek(file_handle, 0, SEEK_END);
		const size_t buffer_size = file_interface->Tell(file_handle);
		file_interface->Seek(file_handle, 0, SEEK_SET);

		using Rml::byte;
		Rml::UniquePtr<byte[]> buffer(new byte[buffer_size]);
		file_interface->Read(buffer.get(), buffer_size, file_handle);
		file_interface->Close(file_handle);

		const size_t i_ext = source.rfind('.');
		Rml::String extension = (i_ext == Rml::String::npos ? Rml::String() : source.substr(i_ext + 1));

#if SDL_MAJOR_VERSION >= 3
		auto CreateSurface = [&]() { return IMG_LoadTyped_IO(SDL_IOFromMem(buffer.get(), int(buffer_size)), 1, extension.c_str()); };
		auto GetSurfaceFormat = [](SDL_Surface* surface) { return surface->format; };
		auto ConvertSurface = [](SDL_Surface* surface, SDL_PixelFormat format) { return SDL_ConvertSurface(surface, format); };
		auto DestroySurface = [](SDL_Surface* surface) { SDL_DestroySurface(surface); };
#else
		auto CreateSurface = [&]() { return IMG_LoadTyped_RW(SDL_RWFromMem(buffer.get(), int(buffer_size)), 1, extension.c_str()); };
		auto GetSurfaceFormat = [](SDL_Surface* surface) { return surface->format->format; };
		auto ConvertSurface = [](SDL_Surface* surface, Uint32 format) { return SDL_ConvertSurfaceFormat(surface, format, 0); };
		auto DestroySurface = [](SDL_Surface* surface) { SDL_FreeSurface(surface); };
#endif

		SDL_Surface* surface = CreateSurface();
		if (!surface)
			return {};

		texture_dimensions = {surface->w, surface->h};

		if (GetSurfaceFormat(surface) != SDL_PIXELFORMAT_RGBA32)
		{
			// Ensure correct format for premultiplied alpha conversion and GenerateTexture below.
			SDL_Surface* converted_surface = ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
			DestroySurface(surface);
			if (!converted_surface)
				return {};

			surface = converted_surface;
		}

		// Convert colors to premultiplied alpha, which is necessary for correct alpha compositing.
		const size_t pixels_byte_size = surface->w * surface->h * 4;
		byte* pixels = static_cast<byte*>(surface->pixels);
		for (size_t i = 0; i < pixels_byte_size; i += 4)
		{
			const byte alpha = pixels[i + 3];
			for (size_t j = 0; j < 3; ++j)
				pixels[i + j] = byte(int(pixels[i + j]) * int(alpha) / 255);
		}

		Rml::TextureHandle texture_handle = RenderInterface_GL3::GenerateTexture({pixels, pixels_byte_size}, texture_dimensions);

		DestroySurface(surface);

		return texture_handle;
	}
};

/**
    Global data used by this backend.

    Lifetime governed by the calls to Backend::Initialize() and Backend::Shutdown().
 */
struct BackendData {
	SystemInterface_SDL system_interface;
	RenderInterface_GL3_SDL render_interface;

	SDL_Window* window = nullptr;
	SDL_GLContext glcontext = nullptr;
	unsigned int uikit_framebuffer = 0;
	unsigned int uikit_renderbuffer = 0;

	bool running = true;
	bool force_next_frame = false;
};
static Rml::UniquePtr<BackendData> data;
#if SDL_MAJOR_VERSION >= 3
static PreProcessEventCallback g_pre_process_event = nullptr;
static LiveResizeRedrawCallback g_live_resize_redraw = nullptr;
static Rml::Context* g_live_resize_context = nullptr;
static bool g_in_live_resize_redraw = false;
static int g_live_resize_last_pixel_w = 0;
static int g_live_resize_last_pixel_h = 0;

// PollEvent/WaitEvent block during interactive resize on several OSes; SDL still
// delivers events to watches. Redraw from here so the compositor does not stretch
// the last frame (see SDL wiki AppFreezeDuringDrag).
static bool SDLCALL LiveResizeEventWatch(void* /*userdata*/, SDL_Event* event)
{
	if (!data || !g_live_resize_redraw || !g_live_resize_context || !event || g_in_live_resize_redraw)
		return true;

	switch (event->type)
	{
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
	case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
	case SDL_EVENT_WINDOW_EXPOSED:
		if (event->window.windowID != SDL_GetWindowID(data->window))
			return true;
		break;
	default:
		return true;
	}

	bool size_may_have_changed = false;
	bool live_expose = false;
	switch (event->type)
	{
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
	case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
		size_may_have_changed = true;
		break;
	case SDL_EVENT_WINDOW_EXPOSED:
		// data1 == 1: live-resize expose while the modal resize/drag loop is active.
		live_expose = (event->window.data1 == 1);
		break;
	default:
		break;
	}

	if (!size_may_have_changed && !live_expose)
		return true;

	int pixel_w = 0;
	int pixel_h = 0;
	SDL_GetWindowSizeInPixels(data->window, &pixel_w, &pixel_h);
	const bool same_pixels = (pixel_w == g_live_resize_last_pixel_w && pixel_h == g_live_resize_last_pixel_h);
	// Coalesce RESIZED + PIXEL_SIZE_CHANGED + follow-up EXPOSED for the same size.
	// Still redraw on display-scale / safe-area when pixels are unchanged.
	if (same_pixels)
	{
		if (live_expose)
			return true;
		if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event->type == SDL_EVENT_WINDOW_RESIZED)
			return true;
	}

	g_in_live_resize_redraw = true;
	g_live_resize_redraw(g_live_resize_context);
	g_live_resize_last_pixel_w = pixel_w;
	g_live_resize_last_pixel_h = pixel_h;
	g_in_live_resize_redraw = false;
	return true;
}
#endif
// Custom SDL user event that wakes SDL_WaitEventTimeout when UI work is posted.
static Uint32 g_wake_event_type = 0;
static std::atomic<bool> g_wake_pending{false};

bool Backend::Initialize(const char* window_name, int width, int height, bool allow_resize, bool borderless)
{
	RMLUI_ASSERT(!data);

#if SDL_MAJOR_VERSION >= 3
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return false;
#else
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
		return false;
#endif

	g_wake_event_type = SDL_RegisterEvents(1);
	g_wake_pending.store(false, std::memory_order_relaxed);

	// Submit click events when focusing the window.
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
	// Touch events are handled natively, no need to generate synthetic mouse events for touch devices.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

#if defined RMLUI_BACKEND_SIMULATE_TOUCH
	// Simulate touch events from mouse events for testing touch behavior on a desktop machine.
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif

	MobileGlLifecycle::ConfigureSdlGlAttributes();
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#if SDL_MAJOR_VERSION >= 3
	const float window_size_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, window_name);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, int(width * window_size_scale));
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, int(height * window_size_scale));
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, allow_resize);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, borderless);
	// macOS desktop: transparent buffer so CALayer-rounded corners composite to the
	// desktop instead of opaque black (see DesktopWindowChrome::RefreshAppearance).
#if defined(__APPLE__) && !TARGET_OS_IPHONE
	if (borderless) {
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);
	}
#endif
	MobileGlLifecycle::SetMobileWindowCreateProperties(props);
	SDL_Window* window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
#else
	Uint32 window_flags = (SDL_WINDOW_OPENGL | (allow_resize ? SDL_WINDOW_RESIZABLE : 0));
	if (borderless) {
		window_flags |= SDL_WINDOW_BORDERLESS;
	}
	SDL_Window* window = SDL_CreateWindow(window_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, window_flags);
	// SDL2 implicitly activates text input on window creation. Turn it off for now, it will be activated again e.g. when focusing a text input field.
	SDL_StopTextInput();
#endif

	if (!window)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create window: %s", SDL_GetError());
		return false;
	}

	SDL_GLContext glcontext = SDL_GL_CreateContext(window);
	if (!glcontext)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create GL context: %s", SDL_GetError());
		SDL_DestroyWindow(window);
		return false;
	}
	if (!SDL_GL_MakeCurrent(window, glcontext))
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on MakeCurrent: %s", SDL_GetError());
		SDL_GL_DestroyContext(glcontext);
		SDL_DestroyWindow(window);
		return false;
	}
	SDL_GL_SetSwapInterval(1);

	if (!RmlGL3::Initialize())
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize OpenGL renderer");
		return false;
	}

	data = Rml::MakeUnique<BackendData>();

	if (!data->render_interface)
	{
		data.reset();
		Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize OpenGL3 render interface");
		return false;
	}

	data->window = window;
	data->glcontext = glcontext;

	data->system_interface.SetWindow(window);
	data->render_interface.SetViewport(width, height);

#if SDL_MAJOR_VERSION >= 3
	{
		int pixel_w = 0;
		int pixel_h = 0;
		SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
		data->render_interface.SetViewport(pixel_w, pixel_h);
	}
	MobileGlLifecycle::InitIosDrawableFromWindow(window, data->uikit_framebuffer, data->uikit_renderbuffer);
	if (data->uikit_framebuffer != 0) {
		data->render_interface.SetOutputFramebuffer(data->uikit_framebuffer);
		Rml::Log::Message(Rml::Log::LT_ERROR, "iOS drawable FBO=%u RBO=%u", data->uikit_framebuffer,
			data->uikit_renderbuffer);
	}
#endif

	TouchSimOverlay::Initialize(window);

#if SDL_MAJOR_VERSION >= 3
	SDL_AddEventWatch(LiveResizeEventWatch, nullptr);
#endif

	return true;
}

void Backend::SyncContext(Rml::Context* context)
{
	if (!data || !context)
		return;

#if SDL_MAJOR_VERSION >= 3
	int pixel_w = 0;
	int pixel_h = 0;
	SDL_GetWindowSizeInPixels(data->window, &pixel_w, &pixel_h);
	context->SetDimensions(Rml::Vector2i(pixel_w, pixel_h));
	context->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(data->window));
	data->render_interface.SetViewport(pixel_w, pixel_h);
	data->force_next_frame = true;
	Rml::Log::Message(Rml::Log::LT_DEBUG, "SyncContext: %dx%d scale=%.3f", pixel_w, pixel_h,
		SDL_GetWindowDisplayScale(data->window));
#else
	(void)context;
#endif
}

#if SDL_MAJOR_VERSION >= 3
void Backend::SetPreProcessEventHandler(PreProcessEventCallback callback)
{
	g_pre_process_event = callback;
}

void Backend::SetLiveResizeHandler(Rml::Context* context, LiveResizeRedrawCallback callback)
{
	g_live_resize_context = context;
	g_live_resize_redraw = callback;
	g_live_resize_last_pixel_w = 0;
	g_live_resize_last_pixel_h = 0;
}

SDL_Window* Backend::GetWindow()
{
	return data ? data->window : nullptr;
}

void Backend::RecoverAfterDeviceReset(Rml::Context* context)
{
	if (!data)
		return;

	SDL_GLContext current = SDL_GL_GetCurrentContext();
	if (current)
		data->glcontext = current;
	else if (data->glcontext)
		SDL_GL_MakeCurrent(data->window, data->glcontext);

	Rml::Log::Message(Rml::Log::LT_WARNING, "Recovering GPU resources after RENDER_DEVICE_RESET");

	// Drop RmlUi-owned GPU caches first (stale GL names), then rebuild renderer objects.
	Rml::ReleaseTextures(&data->render_interface);
	Rml::ReleaseCompiledGeometry(&data->render_interface);
	Rml::ReleaseFontResources();

	TextLoupeRenderer::ReleaseGpuResources();
	data->render_interface.RecoverGpuResources();

	MobileGlLifecycle::UpdateIosDrawableFromWindow(data->window, data->uikit_framebuffer, data->uikit_renderbuffer);
	if (data->uikit_framebuffer != 0) {
		data->render_interface.SetOutputFramebuffer(data->uikit_framebuffer);
	}

	if (context)
		SyncContext(context);

	data->force_next_frame = true;
}
#endif

bool Backend::CanRender()
{
	if (!data || !data->window || !data->glcontext)
		return false;

#if SDL_MAJOR_VERSION >= 3
	// iOS/Android can drop the current context across lifecycle events.
	if (SDL_GL_GetCurrentContext() != data->glcontext)
	{
		if (!SDL_GL_MakeCurrent(data->window, data->glcontext))
			return false;
	}

	int pixel_w = 0;
	int pixel_h = 0;
	SDL_GetWindowSizeInPixels(data->window, &pixel_w, &pixel_h);
	return pixel_w > 0 && pixel_h > 0;
#else
	return true;
#endif
}

void Backend::Shutdown()
{
	RMLUI_ASSERT(data);

	TouchSimOverlay::Shutdown();

#if SDL_MAJOR_VERSION >= 3
	SDL_RemoveEventWatch(LiveResizeEventWatch, nullptr);
	g_live_resize_redraw = nullptr;
	g_live_resize_context = nullptr;
	g_pre_process_event = nullptr;

	SDL_GL_MakeCurrent(data->window, nullptr);
	SDL_GL_DestroyContext(data->glcontext);
#else
	SDL_GL_DeleteContext(data->glcontext);
#endif

	SDL_DestroyWindow(data->window);

	data.reset();
	g_wake_event_type = 0;
	g_wake_pending.store(false, std::memory_order_relaxed);

	SDL_Quit();
}

Rml::SystemInterface* Backend::GetSystemInterface()
{
	RMLUI_ASSERT(data);
	return &data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface()
{
	RMLUI_ASSERT(data);
	return &data->render_interface;
}

bool Backend::ProcessEvents(Rml::Context* context, KeyDownCallback key_down_callback, bool power_save)
{
	RMLUI_ASSERT(data && context);

#if defined RMLUI_PLATFORM_EMSCRIPTEN

	// Ideally we would hand over control of the main loop to emscripten:
	//
	//  // Hand over control of the main loop to the WebAssembly runtime.
	//  emscripten_set_main_loop_arg(EventLoopIteration, (void*)user_data_handle, 0, true);
	//
	// The above is the recommended approach. However, as we don't control the main loop here we have to make due with another approach. Instead, use
	// Asyncify to yield by sleeping.
	// Important: Must be linked with option -sASYNCIFY
	emscripten_sleep(1);

#endif

#if SDL_MAJOR_VERSION >= 3
	#define RMLSDL_WINDOW_EVENTS_BEGIN
	#define RMLSDL_WINDOW_EVENTS_END
	auto GetKey = [](const SDL_Event& event) { return event.key.key; };
	auto GetDisplayScale = []() { return SDL_GetWindowDisplayScale(data->window); };
	constexpr auto event_quit = SDL_EVENT_QUIT;
	constexpr auto event_key_down = SDL_EVENT_KEY_DOWN;
	constexpr auto event_window_size_changed = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
	bool has_event = false;
#else
	#define RMLSDL_WINDOW_EVENTS_BEGIN \
	case SDL_WINDOWEVENT:              \
	{                                  \
		switch (ev.window.event)       \
		{
	#define RMLSDL_WINDOW_EVENTS_END \
		}                            \
		}                            \
		break;
	auto GetKey = [](const SDL_Event& event) { return event.key.keysym.sym; };
	auto GetDisplayScale = []() { return 1.f; };
	constexpr auto event_quit = SDL_QUIT;
	constexpr auto event_key_down = SDL_KEYDOWN;
	constexpr auto event_window_size_changed = SDL_WINDOWEVENT_SIZE_CHANGED;
	int has_event = 0;
#endif

	// Exit without blocking in power-save wait — RequestExit / quit must feel instant.
	if (!data->running) {
		return false;
	}

	bool result = true;

	const bool force_frame = data->force_next_frame;
	data->force_next_frame = false;

	SDL_Event ev;
	if (force_frame)
		has_event = SDL_PollEvent(&ev);
	else if (power_save) {
		// Cap idle wait so main-loop work (relay poll tick, libp2p tick, badge refresh) is not
		// starved until the next touch. Foreground inbox poll is 2s; the old 10s cap made Android
		// (and idle desktop) feel like notifications only appear after interaction.
		constexpr double k_max_power_save_wait_sec = 2.0;
		const double delay_sec = Rml::Math::Min(context->GetNextUpdateDelay(), k_max_power_save_wait_sec);
		has_event = SDL_WaitEventTimeout(&ev, static_cast<int>(delay_sec * 1000));
	} else
		has_event = SDL_PollEvent(&ev);

	while (has_event)
	{
		bool propagate_event = true;
		if (g_wake_event_type != 0 && ev.type == g_wake_event_type) {
			g_wake_pending.store(false, std::memory_order_relaxed);
			has_event = SDL_PollEvent(&ev);
			continue;
		}
#if SDL_MAJOR_VERSION >= 3
		if (g_pre_process_event && g_pre_process_event(context, ev, propagate_event)) {
			has_event = SDL_PollEvent(&ev);
			continue;
		}
#endif
		switch (ev.type)
		{
		case event_quit:
		{
			propagate_event = false;
			result = false;
			data->running = false;
		}
		break;
		case event_key_down:
		{
			propagate_event = false;
#if SDL_MAJOR_VERSION >= 3
			if (GetKey(ev) == SDLK_ESCAPE || GetKey(ev) == SDLK_AC_BACK) {
				if (g_pre_process_event && g_pre_process_event(context, ev, propagate_event)) {
					break;
				}
			}
#endif
			const Rml::Input::KeyIdentifier key = RmlSDL::ConvertKey(GetKey(ev));
			const int key_modifier = RmlSDL::GetKeyModifierState();
			const float native_dp_ratio = GetDisplayScale();

			// See if we have any global shortcuts that take priority over the context.
			if (key_down_callback && !key_down_callback(context, key, key_modifier, native_dp_ratio, true))
				break;
			// Otherwise, hand the event over to the context by calling the input handler as normal.
			if (!RmlSDL::InputEventHandler(context, data->window, ev))
				break;
			// The key was not consumed by the context either, try keyboard shortcuts of lower priority.
			if (key_down_callback && !key_down_callback(context, key, key_modifier, native_dp_ratio, false))
				break;
		}
		break;

#if SDL_MAJOR_VERSION >= 3
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
			SyncContext(context);
			propagate_event = false;
			break;
		case SDL_EVENT_RENDER_DEVICE_RESET:
			RecoverAfterDeviceReset(context);
			propagate_event = false;
			break;
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_FOREGROUND:
		case SDL_EVENT_LOW_MEMORY:
			if (g_pre_process_event) {
				g_pre_process_event(context, ev, propagate_event);
			}
			propagate_event = false;
			break;
#endif

			RMLSDL_WINDOW_EVENTS_BEGIN

		case event_window_size_changed:
			SyncContext(context);
			propagate_event = false;
			break;

			RMLSDL_WINDOW_EVENTS_END

		default: break;
		}

		if (propagate_event)
			RmlSDL::InputEventHandler(context, data->window, ev);

		has_event = SDL_PollEvent(&ev);
	}

	return result && data->running;
}

void Backend::RequestExit()
{
	RMLUI_ASSERT(data);

	data->running = false;
	// Unblock SDL_WaitEventTimeout in ProcessEvents so titlebar close is not capped
	// by the power-save idle wait (up to 2s).
	WakeEventLoop();
}

void Backend::WakeEventLoop()
{
	if (!data || g_wake_event_type == 0)
		return;

	// Coalesce: one pending wake is enough to drain any number of queued UI tasks.
	bool expected = false;
	if (!g_wake_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return;

	SDL_Event ev{};
	ev.type = g_wake_event_type;
#if SDL_MAJOR_VERSION >= 3
	if (!SDL_PushEvent(&ev))
#else
	if (SDL_PushEvent(&ev) < 0)
#endif
	{
		g_wake_pending.store(false, std::memory_order_relaxed);
	}
}

void Backend::BeginFrame()
{
	RMLUI_ASSERT(data);
#if SDL_MAJOR_VERSION >= 3
	if (data->glcontext && SDL_GL_GetCurrentContext() != data->glcontext)
		SDL_GL_MakeCurrent(data->window, data->glcontext);
#endif

	data->render_interface.Clear();
	data->render_interface.BeginFrame();
}

void Backend::PresentFrame()
{
	RMLUI_ASSERT(data);

	data->render_interface.EndFrame();

#if SDL_MAJOR_VERSION >= 3
	{
		int pixel_w = 0;
		int pixel_h = 0;
		SDL_GetWindowSizeInPixels(data->window, &pixel_w, &pixel_h);
		TouchSimOverlay::Draw(data->window, pixel_w, pixel_h);
	}
#endif

	MobileGlLifecycle::BindIosPresentTargets(data->uikit_framebuffer, data->uikit_renderbuffer);

	SDL_GL_SwapWindow(data->window);

	// Optional, used to mark frames during performance profiling.
	RMLUI_FrameMark;
}
