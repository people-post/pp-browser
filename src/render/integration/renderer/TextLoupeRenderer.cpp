#include "TextLoupeRenderer.h"

#include "RmlUi_Renderer_GL3.h"

#if defined(RMLUI_PLATFORM_EMSCRIPTEN) || defined(__ANDROID__)
	#define LOUPE_SHADER_HEADER "#version 300 es\nprecision highp float;\n"
	#include <GLES3/gl3.h>
#else
	#define LOUPE_SHADER_HEADER "#version 330 core\n"
	#include "RmlUi_Include_GL3.h"
#endif

#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <vector>

namespace {

constexpr float kLoupeRadiusDp = 60.f;
constexpr float kLoupeOffsetYDp = 55.f;
constexpr float kLoupeZoom = 2.f;
constexpr float kLoupeEdgeSoftness = 0.04f;

struct LoupeGpuState {
	bool initialized = false;
	GLuint capture_framebuffer = 0;
	GLuint capture_texture = 0;
	int capture_texture_size = 0;

	GLuint program = 0;
	GLuint shadow_program = 0;
	GLuint vao = 0;
	GLuint vbo = 0;
	GLint uniform_projection = -1;
	GLint uniform_center = -1;
	GLint uniform_radius = -1;
	GLint uniform_capture_texture = -1;
	GLint uniform_zoom = -1;
	GLint uniform_edge_softness = -1;
	GLint uniform_shadow_center = -1;
	GLint uniform_shadow_radius = -1;
	GLint uniform_shadow_color = -1;
};

LoupeGpuState g_state;

const char* kVertexShader = LOUPE_SHADER_HEADER R"(
layout(location = 0) in vec2 in_position;
uniform mat4 u_projection;
out vec2 v_screen_pos;
void main() {
	v_screen_pos = in_position;
	gl_Position = u_projection * vec4(in_position, 0.0, 1.0);
}
)";

const char* kLoupeFragmentShader = LOUPE_SHADER_HEADER R"(
uniform vec2 u_center;
uniform float u_radius;
uniform sampler2D u_capture_texture;
uniform float u_zoom;
uniform float u_edge_softness;
in vec2 v_screen_pos;
out vec4 frag_color;

void main() {
	vec2 delta = v_screen_pos - u_center;
	float dist = length(delta);
	float alpha = 1.0 - smoothstep(u_radius * (1.0 - u_edge_softness), u_radius, dist);
	if (alpha <= 0.0)
		discard;

	vec2 local = delta / u_radius;
	vec2 uv = vec2(0.5 + 0.5 * local.x / u_zoom, 0.5 - 0.5 * local.y / u_zoom);
	vec3 color = texture(u_capture_texture, uv).rgb;

	float rim = smoothstep(u_radius - 2.0, u_radius - 1.0, dist) * (1.0 - smoothstep(u_radius - 1.0, u_radius, dist));
	color = mix(color, vec3(1.0), rim);

	frag_color = vec4(color, alpha);
}
)";

const char* kShadowFragmentShader = LOUPE_SHADER_HEADER R"(
uniform vec2 u_center;
uniform float u_radius;
uniform vec4 u_color;
in vec2 v_screen_pos;
out vec4 frag_color;

void main() {
	float dist = length(v_screen_pos - u_center);
	float alpha = u_color.a * (1.0 - smoothstep(u_radius * 0.85, u_radius, dist));
	if (alpha <= 0.0)
		discard;
	frag_color = vec4(u_color.rgb, alpha);
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

void EnsureCaptureTarget(int texture_size)
{
	texture_size = std::max(texture_size, 1);
	if (g_state.initialized && g_state.capture_texture_size == texture_size)
		return;

	if (g_state.capture_framebuffer)
	{
		glDeleteFramebuffers(1, &g_state.capture_framebuffer);
		g_state.capture_framebuffer = 0;
	}
	if (g_state.capture_texture)
	{
		glDeleteTextures(1, &g_state.capture_texture);
		g_state.capture_texture = 0;
	}

	glGenTextures(1, &g_state.capture_texture);
	glBindTexture(GL_TEXTURE_2D, g_state.capture_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture_size, texture_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &g_state.capture_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, g_state.capture_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_state.capture_texture, 0);
#if defined(RMLUI_PLATFORM_EMSCRIPTEN) || defined(__ANDROID__)
	{
		const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &draw_buffer);
	}
#endif
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "TextLoupe capture framebuffer is incomplete.");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	g_state.capture_texture_size = texture_size;
}

void EnsurePrograms()
{
	if (g_state.program != 0)
		return;

	const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	g_state.program = LinkProgram(vertex_shader, CompileShader(GL_FRAGMENT_SHADER, kLoupeFragmentShader));
	g_state.shadow_program = LinkProgram(CompileShader(GL_VERTEX_SHADER, kVertexShader), CompileShader(GL_FRAGMENT_SHADER, kShadowFragmentShader));

	g_state.uniform_projection = glGetUniformLocation(g_state.program, "u_projection");
	g_state.uniform_center = glGetUniformLocation(g_state.program, "u_center");
	g_state.uniform_radius = glGetUniformLocation(g_state.program, "u_radius");
	g_state.uniform_capture_texture = glGetUniformLocation(g_state.program, "u_capture_texture");
	g_state.uniform_zoom = glGetUniformLocation(g_state.program, "u_zoom");
	g_state.uniform_edge_softness = glGetUniformLocation(g_state.program, "u_edge_softness");

	g_state.uniform_shadow_center = glGetUniformLocation(g_state.shadow_program, "u_center");
	g_state.uniform_shadow_radius = glGetUniformLocation(g_state.shadow_program, "u_radius");
	g_state.uniform_shadow_color = glGetUniformLocation(g_state.shadow_program, "u_color");

	glGenVertexArrays(1, &g_state.vao);
	glGenBuffers(1, &g_state.vbo);
	glBindVertexArray(g_state.vao);
	glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
	glBindVertexArray(0);

	g_state.initialized = true;
}

void BuildCircleMesh(float center_x, float center_y, float radius, int segments, std::vector<float>& out_vertices)
{
	out_vertices.clear();
	out_vertices.reserve(size_t(segments) * 4);
	for (int i = 0; i < segments; ++i)
	{
		const float angle0 = float(i) / float(segments) * Rml::Math::RMLUI_PI * 2.f;
		const float angle1 = float(i + 1) / float(segments) * Rml::Math::RMLUI_PI * 2.f;
		const float x0 = center_x + Rml::Math::Cos(angle0) * radius;
		const float y0 = center_y + Rml::Math::Sin(angle0) * radius;
		const float x1 = center_x + Rml::Math::Cos(angle1) * radius;
		const float y1 = center_y + Rml::Math::Sin(angle1) * radius;
		out_vertices.push_back(center_x);
		out_vertices.push_back(center_y);
		out_vertices.push_back(x0);
		out_vertices.push_back(y0);
		out_vertices.push_back(x1);
		out_vertices.push_back(y1);
	}
}

Rml::Rectanglei ComputeCaptureRegion(const Rml::TextLoupeState& state, float dp_ratio, int viewport_width, int viewport_height, int& out_capture_size)
{
	const float radius_px = kLoupeRadiusDp * dp_ratio;
	const int capture_size = Rml::Math::Max(1, Rml::Math::RoundUpToInteger((radius_px * 2.f) / kLoupeZoom));
	out_capture_size = capture_size;

	const int center_x = int(state.anchor.x);
	const int center_y = int(state.anchor.y);
	Rml::Rectanglei region;
	region.p0.x = center_x - capture_size / 2;
	region.p0.y = center_y - capture_size / 2;
	region.p1.x = region.p0.x + capture_size;
	region.p1.y = region.p0.y + capture_size;

	region.p0.x = Rml::Math::Clamp(region.p0.x, 0, viewport_width);
	region.p0.y = Rml::Math::Clamp(region.p0.y, 0, viewport_height);
	region.p1.x = Rml::Math::Clamp(region.p1.x, 0, viewport_width);
	region.p1.y = Rml::Math::Clamp(region.p1.y, 0, viewport_height);
	return region;
}

float ComputeLoupeCenterY(const Rml::TextLoupeState& state, float dp_ratio, int viewport_height)
{
	const float center_y = state.anchor.y - (kLoupeOffsetYDp * dp_ratio);
	return Rml::Math::Clamp(center_y, kLoupeRadiusDp * dp_ratio, float(viewport_height) - kLoupeRadiusDp * dp_ratio);
}

void DrawCircle(GLuint program, GLint projection_location, GLint center_location, GLint radius_location, GLint color_location,
	const float projection[16], Rml::Vector2f center, float radius, float r, float g, float b, float a)
{
	static std::vector<float> vertices;
	BuildCircleMesh(center.x, center.y, radius, 48, vertices);

	glUseProgram(program);
	glUniformMatrix4fv(projection_location, 1, GL_FALSE, projection);
	glUniform2f(center_location, center.x, center.y);
	glUniform1f(radius_location, radius);
	if (color_location >= 0)
		glUniform4f(color_location, r, g, b, a);

	glBindVertexArray(g_state.vao);
	glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizei(vertices.size() * sizeof(float)), vertices.data(), GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size() / 2));
	glBindVertexArray(0);
}

} // namespace

namespace TextLoupeRenderer {

void ReleaseGpuResources()
{
	// After EGL context loss, GL names are invalid; drop state without glDelete*.
	g_state = {};
}

void Render(Rml::TextLoupePhase phase, const Rml::TextLoupeState& state, RenderInterface_GL3& renderer, float dp_ratio)
{
	if (!state.active)
		return;

	EnsurePrograms();

	const int viewport_width = renderer.GetViewportWidth();
	const int viewport_height = renderer.GetViewportHeight();
	int capture_size = 0;
	const Rml::Rectanglei capture_region = ComputeCaptureRegion(state, dp_ratio, viewport_width, viewport_height, capture_size);
	if (!capture_region.Valid())
		return;

	EnsureCaptureTarget(capture_size);

	if (phase == Rml::TextLoupePhase::Capture)
	{
		renderer.BlitTopLayerRegion(capture_region, g_state.capture_framebuffer, capture_size, capture_size);
		return;
	}

	renderer.BindTopLayerFramebuffer();

	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	const float projection[16] = {
		2.f / float(viewport_width), 0.f, 0.f, 0.f,
		0.f, -2.f / float(viewport_height), 0.f, 0.f,
		0.f, 0.f, -1.f, 0.f,
		-1.f, 1.f, 0.f, 1.f,
	};

	const float radius_px = kLoupeRadiusDp * dp_ratio;
	const Rml::Vector2f loupe_center(state.anchor.x, ComputeLoupeCenterY(state, dp_ratio, viewport_height));

	DrawCircle(g_state.shadow_program, g_state.uniform_projection, g_state.uniform_shadow_center, g_state.uniform_shadow_radius,
		g_state.uniform_shadow_color, projection, loupe_center, radius_px + 4.f * dp_ratio, 0.f, 0.f, 0.f, 0.28f);

	glUseProgram(g_state.program);
	glUniformMatrix4fv(g_state.uniform_projection, 1, GL_FALSE, projection);
	glUniform2f(g_state.uniform_center, loupe_center.x, loupe_center.y);
	glUniform1f(g_state.uniform_radius, radius_px);
	glUniform1f(g_state.uniform_zoom, kLoupeZoom);
	glUniform1f(g_state.uniform_edge_softness, kLoupeEdgeSoftness);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_state.capture_texture);
	glUniform1i(g_state.uniform_capture_texture, 0);

	static std::vector<float> vertices;
	BuildCircleMesh(loupe_center.x, loupe_center.y, radius_px, 48, vertices);
	glBindVertexArray(g_state.vao);
	glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
	glBufferData(GL_ARRAY_BUFFER, GLsizei(vertices.size() * sizeof(float)), vertices.data(), GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size() / 2));
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace TextLoupeRenderer
