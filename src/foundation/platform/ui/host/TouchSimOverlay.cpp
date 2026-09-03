#include "TouchSimOverlay.h"

#include "RmlUi_Include_GL3.h"

#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Vector2.h>

#include <vector>

#include <SDL3/SDL.h>

namespace {

#if defined(RMLUI_BACKEND_SIMULATE_TOUCH)

struct OverlayState {
	bool initialized = false;

	GLuint program = 0;
	GLuint vao = 0;
	GLuint vbo = 0;
	GLint uniform_projection = -1;
	GLint uniform_color = -1;

	std::vector<float> circle_vertices;
};

OverlayState g_state;

const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 in_position;
uniform mat4 u_projection;
void main() {
	gl_Position = u_projection * vec4(in_position, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(
#version 330 core
uniform vec4 u_color;
out vec4 frag_color;
void main() {
	frag_color = u_color;
}
)";

GLuint CompileShader(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	return shader;
}

GLuint LinkProgram(GLuint vertex_shader, GLuint fragment_shader)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	return program;
}

void BuildCircleTemplate(float radius, int segments)
{
	g_state.circle_vertices.clear();
	g_state.circle_vertices.reserve(size_t(segments) * 2);
	for (int i = 0; i < segments; ++i)
	{
		const float angle = float(i) / float(segments) * Rml::Math::RMLUI_PI * 2.f;
		g_state.circle_vertices.push_back(Rml::Math::Cos(angle) * radius);
		g_state.circle_vertices.push_back(Rml::Math::Sin(angle) * radius);
	}
}

void DrawFilledCircle(Rml::Vector2f center, float radius, const float projection[16], float r, float g, float b, float a)
{
	glUseProgram(g_state.program);
	glUniformMatrix4fv(g_state.uniform_projection, 1, GL_FALSE, projection);
	glUniform4f(g_state.uniform_color, r, g, b, a);

	std::vector<float> vertices;
	vertices.reserve(g_state.circle_vertices.size());
	for (size_t i = 0; i < g_state.circle_vertices.size(); i += 2)
	{
		vertices.push_back(g_state.circle_vertices[i] * radius / 11.f + center.x);
		vertices.push_back(g_state.circle_vertices[i + 1] * radius / 11.f + center.y);
	}

	glBindVertexArray(g_state.vao);
	glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(float)), vertices.data(), GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
	glDrawArrays(GL_TRIANGLE_FAN, 0, GLsizei(vertices.size() / 2));
}

void MakeProjectionMatrix(float width, float height, float out[16])
{
	// Top-left origin, matching RmlUi ProjectOrtho(0, w, h, 0).
	for (int i = 0; i < 16; ++i)
		out[i] = 0.f;
	out[0] = 2.f / width;
	out[5] = -2.f / height;
	out[10] = -1.f;
	out[12] = -1.f;
	out[13] = 1.f;
	out[15] = 1.f;
}

Rml::Vector2f GetSimulatedTouchPosition(SDL_Window* window)
{
	float mouse_x = 0.f;
	float mouse_y = 0.f;
	SDL_GetMouseState(&mouse_x, &mouse_y);

	int window_w = 0;
	int window_h = 0;
	int pixel_w = 0;
	int pixel_h = 0;
	SDL_GetWindowSize(window, &window_w, &window_h);
	SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);

	if (window_w <= 0 || window_h <= 0)
		return {};

	// Match TouchEventToTouchList: normalized finger coords × pixel dimensions.
	return Rml::Vector2f{(mouse_x / float(window_w)) * float(pixel_w), (mouse_y / float(window_h)) * float(pixel_h)};
}

#endif // RMLUI_BACKEND_SIMULATE_TOUCH

} // namespace

void TouchSimOverlay::Initialize(SDL_Window* /*window*/)
{
#if defined(RMLUI_BACKEND_SIMULATE_TOUCH)
	if (g_state.initialized)
		return;

	SDL_HideCursor();

	GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
	g_state.program = LinkProgram(vertex_shader, fragment_shader);
	g_state.uniform_projection = glGetUniformLocation(g_state.program, "u_projection");
	g_state.uniform_color = glGetUniformLocation(g_state.program, "u_color");

	glGenVertexArrays(1, &g_state.vao);
	glGenBuffers(1, &g_state.vbo);
	BuildCircleTemplate(11.f, 24);

	g_state.initialized = true;
#else
	(void)0;
#endif
}

void TouchSimOverlay::Shutdown()
{
#if defined(RMLUI_BACKEND_SIMULATE_TOUCH)
	if (!g_state.initialized)
		return;

	SDL_ShowCursor();

	glDeleteProgram(g_state.program);
	glDeleteVertexArrays(1, &g_state.vao);
	glDeleteBuffers(1, &g_state.vbo);
	g_state = {};
#endif
}

void TouchSimOverlay::Draw(SDL_Window* window, int viewport_w, int viewport_h)
{
#if defined(RMLUI_BACKEND_SIMULATE_TOUCH)
	if (!g_state.initialized || !window)
		return;

	int pixel_w = 0;
	int pixel_h = 0;
	SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
	if (pixel_w <= 0 || pixel_h <= 0)
		return;

	// Prefer live pixel size so the overlay stays aligned after resize even if
	// PresentFrame passes dimensions from the start of the frame.
	viewport_w = pixel_w;
	viewport_h = pixel_h;

	if (SDL_GetMouseFocus() != window)
		return;

	const Rml::Vector2f center = GetSimulatedTouchPosition(window);

	float projection[16];
	MakeProjectionMatrix(float(viewport_w), float(viewport_h), projection);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, viewport_w, viewport_h);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);

	DrawFilledCircle(center, 11.f, projection, 231.f / 255.f, 76.f / 255.f, 60.f / 255.f, 0.55f);

	glBindVertexArray(0);
	glUseProgram(0);
#else
	(void)window;
	(void)viewport_w;
	(void)viewport_h;
#endif
}
