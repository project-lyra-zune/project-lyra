#include "install_ui.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <zdkgl.h>
#include <zdksystem.h>

#include "device_reboot.h"

// Tegra can't compile GLSL at runtime; shaders load as precompiled NV binaries.
#ifndef GL_NVIDIA_PLATFORM_BINARY_NV
#define GL_NVIDIA_PLATFORM_BINARY_NV 0x890B
#endif

#define CONTENT "\\gametitle\\584E07D1\\Content"

// AccentWarmColor / AccentMagentaColor (Xune LightTheme).
static const GLfloat kWarm[3]    = { 0xE6 / 255.0f, 0x8B / 255.0f, 0x23 / 255.0f };
static const GLfloat kMagenta[3] = { 0xDE / 255.0f, 0x29 / 255.0f, 0x84 / 255.0f };
static const GLfloat kLabel[3]   = { 0.88f, 0.88f, 0.90f };

static const float kCycleMs = 2500.0f;
static const DWORD kRebootDelayMs = 3500;

// ── Shared stage (worker writes, render loop reads) ──────────────────────────

static volatile LONG g_stage = STAGE_PREPARE;

void set_install_stage(int stage) {
	InterlockedExchange(&g_stage, (LONG)stage);
}

static int read_stage() {
	return (int)g_stage;   // aligned 32-bit load: atomic, no lock
}

static void (*g_work)(void) = NULL;

static DWORD WINAPI worker_thunk(LPVOID) {
	if (g_work) g_work();
	return 0;
}

// ── Asset loading ────────────────────────────────────────────────────────────

static bool load_binary_shader(GLuint shader, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* buf = malloc(len);
	bool ok = buf && (long)fread(buf, 1, len, f) == len;
	if (ok) glShaderBinary(1, &shader, GL_NVIDIA_PLATFORM_BINARY_NV, buf, len);
	free(buf);
	fclose(f);
	return ok;
}

static GLuint build_program(const char* vs_path, const char* fs_path) {
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	if (!load_binary_shader(vs, vs_path) || !load_binary_shader(fs, fs_path)) {
		return 0;
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_uv");
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	return linked ? prog : 0;
}

// Coverage rides in the alpha of a white GL_BGRA_EXT texture. Blob: [u16 w][u16 h][BGRA].
static GLuint load_bgra(const char* path, int* out_w, int* out_h) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	unsigned char hdr[4];
	if (fread(hdr, 1, 4, f) != 4) { fclose(f); return 0; }
	int w = hdr[0] | (hdr[1] << 8);
	int h = hdr[2] | (hdr[3] << 8);
	long n = (long)w * h * 4;
	unsigned char* buf = (unsigned char*)malloc(n);
	bool ok = buf && (long)fread(buf, 1, n, f) == n;
	fclose(f);
	if (!ok) { free(buf); return 0; }

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(buf);
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
	return tex;
}

// ── Scene ────────────────────────────────────────────────────────────────────

struct Tex { GLuint id; int w, h; };

static GLuint s_prog;
static GLint  s_uTex, s_uPhase, s_uWash, s_uWarm, s_uMagenta, s_uSolid;
static Tex    s_mask;
static Tex    s_bar;
static Tex    s_labels[STAGE_COUNT];
static int    s_vw, s_vh;   // viewport, queried on the first frame

static const char* kLabelFiles[STAGE_COUNT] = {
	CONTENT "\\status_prepare.bgra",
	CONTENT "\\status_loader.bgra",
	CONTENT "\\status_daemon.bgra",
	CONTENT "\\status_mods.bgra",
	CONTENT "\\status_done.bgra",
};

// Uninstall reuses this scene with its own two labels (see run_uninstall_ui).
static const char* kUninstallLabelFiles[] = {
	CONTENT "\\status_uninstalling.bgra",
	CONTENT "\\status_done.bgra",
};

// Active label set + count, selected by run_install_ui / run_uninstall_ui before setup_gl.
static const char* const* s_label_files = kLabelFiles;
static int                 s_label_count = STAGE_COUNT;

static bool setup_gl() {
	s_prog = build_program(CONTENT "\\wash.nvbv", CONTENT "\\wash.nvbf");
	if (!s_prog) return false;
	glUseProgram(s_prog);
	s_uTex     = glGetUniformLocation(s_prog, "u_tex");
	s_uPhase   = glGetUniformLocation(s_prog, "u_phase");
	s_uWash    = glGetUniformLocation(s_prog, "u_wash");
	s_uWarm    = glGetUniformLocation(s_prog, "u_warm");
	s_uMagenta = glGetUniformLocation(s_prog, "u_magenta");
	s_uSolid   = glGetUniformLocation(s_prog, "u_solid");
	glUniform1i(s_uTex, 0);
	glUniform3fv(s_uWarm, 1, kWarm);
	glUniform3fv(s_uMagenta, 1, kMagenta);
	glUniform3fv(s_uSolid, 1, kLabel);

	s_mask.id = load_bgra(CONTENT "\\lyra_mask.bgra", &s_mask.w, &s_mask.h);
	if (!s_mask.id) return false;
	s_bar.id = load_bgra(CONTENT "\\bar.bgra", &s_bar.w, &s_bar.h);
	if (!s_bar.id) return false;
	for (int i = 0; i < s_label_count; ++i) {
		s_labels[i].id = load_bgra(s_label_files[i], &s_labels[i].w, &s_labels[i].h);
		if (!s_labels[i].id) return false;
	}

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.043f, 0.043f, 0.059f, 1.0f);   // #0b0b0f
	return true;
}

// wash=1 animates the gradient (wordmark); wash=0 fills solid (labels).
static void draw_quad(const Tex& t, float cx, float cy, float w, float h, float wash) {
	float l = cx - w * 0.5f, r = cx + w * 0.5f;
	float tp = cy - h * 0.5f, bt = cy + h * 0.5f;
	float xl = l / s_vw * 2.0f - 1.0f, xr = r / s_vw * 2.0f - 1.0f;
	float yt = 1.0f - tp / s_vh * 2.0f, yb = 1.0f - bt / s_vh * 2.0f;

	const GLfloat pos[8] = { xl, yt,  xl, yb,  xr, yt,  xr, yb };
	const GLfloat uv[8]  = { 0, 0,   0, 1,   1, 0,   1, 1 };

	glUniform1f(s_uWash, wash);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, t.id);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// countdown < 0 while installing, else 0..1 reboot progress.
static void draw_frame(float phase, float countdown) {
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(s_prog);
	glUniform1f(s_uPhase, phase);

	float mw = s_vw * 0.82f;
	float mh = mw * (float)s_mask.h / (float)s_mask.w;
	draw_quad(s_mask, s_vw * 0.5f, s_vh * 0.42f, mw, mh, 1.0f);

	const Tex& lbl = s_labels[read_stage()];
	draw_quad(lbl, s_vw * 0.5f, s_vh * 0.62f, (float)lbl.w, (float)lbl.h, 0.0f);

	if (countdown >= 0.0f) {
		float w = s_vw * 0.5f * countdown;
		draw_quad(s_bar, s_vw * 0.25f + w * 0.5f, s_vh * 0.72f, w, 6.0f, 1.0f);
	}
}

static void render_loop(HANDLE worker) {
	DWORD start = GetTickCount();
	DWORD done_at = 0;

	for (;;) {
		ZDKGL_BeginDraw();
		if (s_vw == 0) {
			GLint vp[4] = { 0, 0, 0, 0 };
			glGetIntegerv(GL_VIEWPORT, vp);
			s_vw = vp[2] ? vp[2] : 272;
			s_vh = vp[3] ? vp[3] : 480;
		}

		if (done_at == 0 && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) {
			done_at = GetTickCount();
		}

		// Crest position, looped 0->2 to match the shader's period; the shader is
		// periodic so the wrap is a seamless one-direction sweep.
		float phase = fmodf((GetTickCount() - start) / kCycleMs, 1.0f) * 2.0f;

		float countdown = -1.0f;
		if (done_at != 0) {
			DWORD elapsed = GetTickCount() - done_at;
			if (elapsed >= kRebootDelayMs) { ZDKGL_EndDraw(); break; }
			countdown = (float)elapsed / kRebootDelayMs;
		}
		draw_frame(phase, countdown);
		ZDKGL_EndDraw();
	}
}

static void run_ui(void (*work)(void), const wchar_t* fallback_msg) {
	// Start the work now so it completes even if GL bring-up fails.
	g_work = work;
	HANDLE worker = CreateThread(NULL, 0, worker_thunk, NULL, 0, NULL);

	ZDKSystem_ShowSplashScreen(false);
	ZDKGL_Initialize();

	if (setup_gl()) {
		render_loop(worker);
		ZDKGL_Cleanup();
	} else {
		// No GL surface: finish the work, report modally.
		ZDKGL_Cleanup();
		if (worker) WaitForSingleObject(worker, INFINITE);
		ZDKSystem_ShowMessageBox(fallback_msg, MESSAGEBOX_TYPE_OK);
	}

	if (worker) CloseHandle(worker);
	RebootDevice();
}

void run_install_ui(void (*work)(void)) {
	s_label_files = kLabelFiles;
	s_label_count = STAGE_COUNT;
	run_ui(work, L"Lyra successfully installed. Rebooting.");
}

void run_uninstall_ui(void (*work)(void)) {
	s_label_files = kUninstallLabelFiles;
	s_label_count = 2;
	run_ui(work, L"Removing Project Lyra. Rebooting.");
}
