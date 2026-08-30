// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Nintendo Switch (Horizon / libnx) native frontend and entry point.
//
// A thin native shell around app::AppSessionController, after the Android port:
// it owns the input pump and watches the session while the guest CPU and the
// render thread run on their own threads. Vulkan and EGL present directly to
// libnx's default NWindow, so SDL video is not used.

#include "switch_state.h"
#include "switch_installer.h"

#include <app/functions.h>
#include <app/state.h>
#include <cpu/functions.h>
#include <ctrl/functions.h>
#include <emuenv/state.h>
#include <ime/keyboard.h>
#include <mem/functions.h>
#include <motion/event_handler.h>
#include <motion/state.h>
#include <audio/state.h>
#include <display/state.h>
#include <io/state.h>
#include <np/trophy/collection.h>
#include <overlay/controls.h>
#include <overlay/display_manager.h>
#include <overlay/pause_overlay.h>
#include <renderer/functions.h>
#include <renderer/vulkan/lsfg.h>
#include <renderer/frame_host.h>
#include <renderer/state.h>
#include <touch/functions.h>
#include <touch/state.h>
#include <util/log.h>
#include <nids/functions.h>
#include <util/switch_thread.h>
#include <util/switch_vibration.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <ctime>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <EGL/egl.h>

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

// libnx NWindow-backed frame host. Vulkan creates its own surface from handle();
// OpenGL and Zink use the EGL context owned here. Vita3K's render thread takes
// ownership after renderer initialization and releases it before teardown.
class SwitchFrameHost final : public renderer::FrameHost {
public:
    explicit SwitchFrameHost(NWindow *nwindow)
        : m_nwindow(nwindow) {}

    ~SwitchFrameHost() override {
        destroy_render_context();
    }

    bool initialize_for_backend(renderer::Backend backend, const std::string &configured_backend) {
        if (m_display != EGL_NO_DISPLAY || m_surface != EGL_NO_SURFACE || m_context != EGL_NO_CONTEXT)
            destroy_render_context();

        if (backend != renderer::Backend::OpenGL) {
            unsetenv("MESA_SWITCH_GLTHREAD");
            unsetenv("MESA_SWITCH_GL_DRIVER");
            unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
            unsetenv("MESA_SHADER_CACHE_DISABLE");
            unsetenv("MESA_SHADER_CACHE_DIR");
            return true;
        }

        const bool use_zink = configured_backend == "Zink";
        // Vita3K already owns a dedicated render thread. Mesa's extra GL thread
        // adds latency and makes context shutdown needlessly complicated.
        setenv("MESA_SWITCH_GLTHREAD", "0", 1);
        setenv("MESA_SWITCH_GL_DRIVER", use_zink ? "zink" : "nvc0", 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", use_zink ? "zink" : "nouveau", 1);
        // Mesa's global Switch cache is shared by every homebrew and its
        // serialized NIR ABI is not versioned, so keep an app-owned versioned
        // one and never delete the global cache. Without it nouveau re-runs the
        // whole nv50_ir compile for every shader on every launch.
        const char *const cache_dir = use_zink
            ? "sdmc:/switch/vita3k/cache/mesa-zink-v1"
            : "sdmc:/switch/vita3k/cache/mesa-nvc0-v1";
        errno = 0;
        if (::mkdir(cache_dir, 0777) == 0 || errno == EEXIST) {
            setenv("MESA_SHADER_CACHE_DIR", cache_dir, 1);
            unsetenv("MESA_SHADER_CACHE_DISABLE");
        } else {
            const int cache_errno = errno;
            unsetenv("MESA_SHADER_CACHE_DIR");
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
            LOG_WARN("Could not create the isolated {} cache (errno {}); Mesa disk cache disabled",
                use_zink ? "Zink" : "nvc0", cache_errno);
        }

        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0;
        EGLint minor = 0;
        if (m_display == EGL_NO_DISPLAY || eglInitialize(m_display, &major, &minor) != EGL_TRUE) {
            LOG_ERROR("Failed to initialize EGL for {} (error {:#x})",
                use_zink ? "Zink" : "OpenGL", static_cast<unsigned>(eglGetError()));
            destroy_render_context();
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
            LOG_ERROR("Failed to bind the desktop OpenGL EGL API (error {:#x})",
                static_cast<unsigned>(eglGetError()));
            destroy_render_context();
            return false;
        }

        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint config_count = 0;
        if (eglChooseConfig(m_display, config_attributes, &config, 1, &config_count) != EGL_TRUE
            || config_count < 1) {
            LOG_ERROR("No compatible Switch OpenGL EGL configuration was found (error {:#x})",
                static_cast<unsigned>(eglGetError()));
            destroy_render_context();
            return false;
        }

        m_surface = eglCreateWindowSurface(m_display, config,
            static_cast<EGLNativeWindowType>(m_nwindow), nullptr);
        if (m_surface == EGL_NO_SURFACE) {
            LOG_ERROR("Failed to create the Switch EGL window surface (error {:#x})",
                static_cast<unsigned>(eglGetError()));
            destroy_render_context();
            return false;
        }

        const EGLint compatibility_45[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        const EGLint compatibility_44[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 4,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, compatibility_45);
        if (m_context == EGL_NO_CONTEXT)
            m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, compatibility_44);
        if (m_context == EGL_NO_CONTEXT)
            m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, nullptr);
        if (m_context == EGL_NO_CONTEXT || !make_current()) {
            LOG_ERROR("Failed to create the Switch OpenGL context (error {:#x})",
                static_cast<unsigned>(eglGetError()));
            destroy_render_context();
            return false;
        }

        LOG_INFO("Initialized {} through EGL {}.{}", use_zink ? "Zink" : "native OpenGL", major, minor);
        return true;
    }

    renderer::DisplayHandle handle() const override {
        return renderer::SwitchDisplayHandle{ m_nwindow };
    }

    // The drawable is the NWindow, not the TV. Its size is whatever libnx gave
    // it (1280x720 unless nwindowSetDimensions is called) and VI scales that to
    // the panel, so it does not follow the operation mode. Reporting the docked
    // mode's 1920x1080 here instead made surface_matches_window_size() false on
    // every frame while docked, which rebuilt the swapchain once per frame.
    void drawable_size(int &width, int &height) const {
        u32 w = 0, h = 0;
        if (m_nwindow && R_SUCCEEDED(nwindowGetDimensions(m_nwindow, &w, &h)) && w && h) {
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            return;
        }
        width = 1280;
        height = 720;
    }

    int drawable_width() const override {
        int width = 0, height = 0;
        drawable_size(width, height);
        return width;
    }

    int drawable_height() const override {
        int width = 0, height = 0;
        drawable_size(width, height);
        return height;
    }

    std::vector<std::string> font_dirs() const override {
        // The Vita firmware font (pfs) lives under the emulated vita fs; the
        // overlay falls back to its built-in glyphs if none is found.
        return {};
    }

    void *get_proc_address(const char *name) const override {
        return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(eglGetProcAddress(name)));
    }

    bool make_current() override {
        if (m_display == EGL_NO_DISPLAY || m_surface == EGL_NO_SURFACE || m_context == EGL_NO_CONTEXT)
            return false;
        if (eglGetCurrentContext() == m_context)
            return true;
        return eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_TRUE;
    }

    void done_current() override {
        if (m_display != EGL_NO_DISPLAY && eglGetCurrentContext() == m_context)
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    void swap_buffers() override {
        if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE)
            eglSwapBuffers(m_display, m_surface);
    }

    bool set_vsync(bool enabled) override {
        if (m_display == EGL_NO_DISPLAY)
            return false;
        return eglSwapInterval(m_display, enabled ? 1 : 0) == EGL_TRUE;
    }

    void prepare_for_render_thread() override {
        done_current();
    }

    void destroy_render_context() override {
        // Keep the Vulkan path completely outside EGL. In the unified Mesa
        // build eglReleaseThread() also reaches shared Nouveau process state;
        // calling it for a frame host that never owned EGL could tear down
        // state immediately before NVK initialized.
        if (m_display == EGL_NO_DISPLAY && m_surface == EGL_NO_SURFACE && m_context == EGL_NO_CONTEXT)
            return;
        done_current();
        if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT) {
            eglDestroyContext(m_display, m_context);
            m_context = EGL_NO_CONTEXT;
        }
        if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE) {
            eglDestroySurface(m_display, m_surface);
            m_surface = EGL_NO_SURFACE;
        }
        if (m_display != EGL_NO_DISPLAY) {
            eglTerminate(m_display);
            m_display = EGL_NO_DISPLAY;
        }
        eglReleaseThread();
    }

private:
    NWindow *m_nwindow = nullptr;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;
};

// Resolve which title to boot: explicit "-r <TITLEID>" argument first, else an
// optional "sdmc:/vita3k/autoboot.txt" containing a title id.
std::string resolve_title_id(int argc, char *argv[]) {
    for (int i = 0; i + 1 < argc; i++) {
        if (std::string(argv[i]) == "-r")
            return argv[i + 1];
    }

    std::ifstream autoboot("sdmc:/switch/vita3k/autoboot.txt");
    if (autoboot) {
        std::string id;
        std::getline(autoboot, id);
        while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' '))
            id.pop_back();
        return id;
    }

    return {};
}

// Console-based title picker, used when Vita3K is started without the SDL
// launcher. consoleExit releases the framebuffer console before the Vulkan
// renderer takes over the NWindow. Returns the chosen title id, or empty.
std::string select_title_from_menu() {
    auto *emuenv = get_emuenv();
    std::vector<app::AppEntry> apps = emuenv ? app::get_apps(*emuenv) : std::vector<app::AppEntry>{};

    consoleInit(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    int selected = 0;
    std::string result;
    bool dirty = true;
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 kdown = padGetButtonsDown(&pad);
        if (kdown & HidNpadButton_Plus)
            break;
        if ((kdown & HidNpadButton_Y) && emuenv) {
            // Install pending files from sdmc:/switch/vita3k/install/, then rescan.
            consoleExit(nullptr);
            run_switch_installer(*emuenv);
            app::init_apps_list(*emuenv);
            apps = app::get_apps(*emuenv);
            selected = 0;
            consoleInit(nullptr);
            dirty = true;
        }
        if (!apps.empty()) {
            if (kdown & HidNpadButton_Down) {
                selected = (selected + 1) % static_cast<int>(apps.size());
                dirty = true;
            }
            if (kdown & HidNpadButton_Up) {
                selected = (selected + static_cast<int>(apps.size()) - 1) % static_cast<int>(apps.size());
                dirty = true;
            }
            if (kdown & HidNpadButton_A) {
                result = apps[selected].title_id;
                break;
            }
        }

        if (dirty) {
            printf("\x1b[2J\x1b[H"); // clear screen + cursor home
            printf("=== Vita3K (Nintendo Switch) ===\n\n");
            if (apps.empty()) {
                printf("No games installed.\n\n");
                printf("To install: put your firmware (.PUP) and/or game\n");
                printf("(.pkg + its work.bin) in:\n");
                printf("  sd:/switch/vita3k/install/\n");
                printf("then press Y to install.\n\n");
                printf("Y: install pending files    +: exit\n");
            } else {
                printf("Select a game:\n\n");
                for (int i = 0; i < static_cast<int>(apps.size()); i++) {
                    printf(" %s %s  [%s]\n", i == selected ? ">" : " ",
                        apps[i].title.c_str(), apps[i].title_id.c_str());
                }
                printf("\nD-pad: move   A: launch   Y: install   +: exit\n");
            }
            dirty = false;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return result;
}

// Poll the Switch touchscreen (libnx HID) and inject front-touch events into the
// guest. The minimal SDL3 Switch backend does not emit SDL_EVENT_FINGER, so we
// synthesize the SDL_TouchFingerEvents that Vita3K's touch layer expects: x/y are
// normalised to the 1280x720 panel and handle_touch_event maps them through the
// render viewport to the Vita's front touchscreen. (The guest must actually read
// touch, e.g. via libsystemgesture from the installed firmware.)
// Stick-driven pointer for games whose menus only answer the front
// touchscreen. Drawn in the overlay's 960x544 Vita-screen space, which is the
// same space the normalized touch coordinates address, so one position serves
// both. Three stacked ellipses keep it readable over any scene.
class VirtualMouseCursor final : public ::overlay::overlay {
public:
    VirtualMouseCursor() {
        visible = true;
        m_halo.back_color = { 0.f, 0.f, 0.f, 0.45f };
        m_body.back_color = k_idle;
        m_dot.back_color = { 0.f, 0.f, 0.f, 0.85f };
        m_halo.set_size(k_halo, k_halo);
        m_body.set_size(k_body, k_body);
        m_dot.set_size(k_dot, k_dot);
        set_position(0.5f, 0.5f);
    }

    void set_position(float nx, float ny) {
        const float px = nx * static_cast<float>(virtual_width);
        const float py = ny * static_cast<float>(virtual_height);
        centre(m_halo, px, py, k_halo);
        centre(m_body, px, py, k_body);
        centre(m_dot, px, py, k_dot);
    }

    void set_pressed(bool pressed) {
        if (pressed == m_pressed)
            return;
        m_pressed = pressed;
        m_body.back_color = pressed ? k_pressed : k_idle;
        m_body.refresh();
    }

    ::overlay::compiled_resource get_compiled() override {
        ::overlay::compiled_resource result;
        if (!visible)
            return result;
        result.add(m_halo.get_compiled());
        result.add(m_body.get_compiled());
        result.add(m_dot.get_compiled());
        return result;
    }

private:
    static constexpr uint16_t k_halo = 22;
    static constexpr uint16_t k_body = 14;
    static constexpr uint16_t k_dot = 5;
    static constexpr ::overlay::color4f k_idle{ 1.f, 1.f, 1.f, 0.95f };
    static constexpr ::overlay::color4f k_pressed{ 0.25f, 0.72f, 1.f, 0.95f };

    static void centre(::overlay::ellipse &shape, float px, float py, uint16_t size) {
        shape.set_pos(static_cast<int16_t>(px - size / 2.0f),
            static_cast<int16_t>(py - size / 2.0f));
    }

    ::overlay::ellipse m_halo;
    ::overlay::ellipse m_body;
    ::overlay::ellipse m_dot;
    bool m_pressed = false;
};

struct VirtualMouseState {
    bool enabled = false;
    float x = 0.5f;
    float y = 0.5f;
    bool pressed = false;
    bool was_pressed = false;
    std::shared_ptr<VirtualMouseCursor> cursor;

    // Well above the panel's own finger ids, which libnx numbers from zero.
    static constexpr uint32_t finger_id = 0x4000;
};

struct SwitchTouchTracker {
    std::array<uint32_t, 8> previous{};
    int previous_count = 0;

    void reset() {
        previous_count = 0;
    }
};

static void switch_poll_touch(EmuEnvState &emuenv, SwitchTouchTracker &tracker, bool accept_input,
    VirtualMouseState &vmouse) {
    static bool inited = false;
    if (!inited) {
        hidInitializeTouchScreen();
        inited = true;
    }

    HidTouchScreenState st{};
    int n = 0;
    if (accept_input && hidGetTouchScreenStates(&st, 1) > 0)
        n = std::min<int>(static_cast<int>(st.count), 8);

    // The Switch handheld touch panel reports in 1280x720 screen space.
    constexpr float kPanelW = 1280.0f;
    constexpr float kPanelH = 720.0f;

    uint32_t cur[8];
    int cur_n = 0;

    const auto inject = [&emuenv](SDL_EventType type, uint32_t finger_id, float x, float y) {
        SDL_TouchFingerEvent f{};
        f.type = type;
        f.touchID = 1; // any non-zero device id
        f.fingerID = static_cast<SDL_FingerID>(finger_id + 1); // avoid id 0
        f.x = x;
        f.y = y;
        f.pressure = 1.0f;
        handle_touch_event(emuenv.touch, f);
    };

    // Down/move for each active touch; track which finger ids are current.
    for (int i = 0; i < n; i++) {
        const uint32_t fid = st.touches[i].finger_id;
        const float x = static_cast<float>(st.touches[i].x) / kPanelW;
        const float y = static_cast<float>(st.touches[i].y) / kPanelH;
        bool was_down = false;
        for (int j = 0; j < tracker.previous_count; j++)
            if (tracker.previous[j] == fid) {
                was_down = true;
                break;
            }
        inject(was_down ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN, fid, x, y);
        cur[cur_n++] = fid;
    }
    // The pointer is one more finger, so a game that tracks touch ids sees a
    // press it cannot tell from a real one. Its id is reserved, so the
    // down/move/up bookkeeping below carries it without a special case.
    const bool pointer_down = vmouse.enabled && vmouse.pressed && accept_input;
    if (pointer_down && cur_n < 8) {
        inject(vmouse.was_pressed ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN,
            VirtualMouseState::finger_id, vmouse.x, vmouse.y);
        cur[cur_n++] = VirtualMouseState::finger_id;
    }
    vmouse.was_pressed = pointer_down;

    // Up for fingers that were down last frame but are gone now.
    for (int j = 0; j < tracker.previous_count; j++) {
        bool still_down = false;
        for (int i = 0; i < cur_n; i++)
            if (cur[i] == tracker.previous[j]) {
                still_down = true;
                break;
            }
        if (!still_down)
            inject(SDL_EVENT_FINGER_UP, tracker.previous[j], 0.0f, 0.0f);
    }
    tracker.previous_count = cur_n;
    for (int i = 0; i < cur_n; i++)
        tracker.previous[i] = cur[i];
}

// SDL's Switch build currently has only the dummy joystick driver, so physical
// controllers never appear in SDL_GetGamepads(). Read the standard libnx pad
// directly and feed it through Vita3K's virtual-keyboard input path, which is
// consumed by both the regular and extended SceCtrl APIs.
// Horizon clamps the sticks to a circle, so the dominant axis only saturates when
// the stick is pushed exactly along it. The Vita reports a square range instead:
// at full deflection its dominant axis reads full whatever the angle. Feeding the
// circle straight through therefore makes a game lose speed as soon as the stick
// leaves an axis. Treat the pair together: radial deadzone, then expand the circle
// back out to the square the guest expects.
static void normalize_stick(const int raw_x, const int raw_y, int deadzone_percent,
    float sensitivity, float &out_x, float &out_y) {
    out_x = 0.0f;
    out_y = 0.0f;

    float x = std::clamp(static_cast<float>(raw_x) / 32767.0f, -1.0f, 1.0f);
    float y = std::clamp(static_cast<float>(raw_y) / 32767.0f, -1.0f, 1.0f);
    const float deadzone = std::clamp(static_cast<float>(deadzone_percent) / 100.0f, 0.0f, 0.90f);

    float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone)
        return;
    if (magnitude > 1.0f) {
        x /= magnitude;
        y /= magnitude;
        magnitude = 1.0f;
    }

    // Radial rather than per-axis, so the direction the stick is pointing survives
    // the deadzone instead of being squared off near the centre.
    const float wanted = (magnitude - deadzone) / (1.0f - deadzone);
    x *= wanted / magnitude;
    y *= wanted / magnitude;

    // FIXME: Sly Cooper still does not read this as a fully analog stick, though
    // the mapping is correct in isolation - likely the guest-side sampling mode.
    //
    // Circle to square (Fernandez-Guasti elliptical grid). Scaling by 1/dominant
    // would stretch the whole disc and cost a 45-degree push up to 41% of extra
    // travel; this is the identity near the centre and opens out only at the rim.
    const float xx = x * x;
    const float yy = y * y;
    const float term = 2.0f * std::numbers::sqrt2_v<float>;
    const auto root = [](float v) { return std::sqrt(std::max(v, 0.0f)); };
    const float sx = 0.5f * root(2.0f + xx - yy + term * x) - 0.5f * root(2.0f + xx - yy - term * x);
    const float sy = 0.5f * root(2.0f - xx + yy + term * y) - 0.5f * root(2.0f - xx + yy - term * y);
    x = sx;
    y = sy;

    const float gain = std::clamp(sensitivity, 0.1f, 3.0f);
    out_x = std::clamp(x * gain, -1.0f, 1.0f);
    out_y = std::clamp(y * gain, -1.0f, 1.0f);
}

static uint32_t vita_face_button(const std::string &name, uint32_t fallback) {
    if (name == "cross")
        return SCE_CTRL_CROSS;
    if (name == "circle")
        return SCE_CTRL_CIRCLE;
    if (name == "triangle")
        return SCE_CTRL_TRIANGLE;
    if (name == "square")
        return SCE_CTRL_SQUARE;
    return fallback;
}

static void clear_switch_controller_input(EmuEnvState &emuenv) {
    std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    emuenv.ctrl.keyboard_state = {};
}

struct SwitchSixAxis {
    std::array<HidSixAxisSensorHandle, 6> handles{};
    std::array<bool, 6> active{};
    bool configured = false;
    u32 configured_style = 0;
    u32 configured_attributes = 0;

    void acquire(const u32 style, const u32 attributes) {
        add(0, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        add(1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);

        HidSixAxisSensorHandle joy_dual[2]{};
        if (R_SUCCEEDED(hidGetSixAxisSensorHandles(joy_dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
            for (int i = 0; i < 2; ++i) {
                handles[2 + i] = joy_dual[i];
                active[2 + i] = R_SUCCEEDED(hidStartSixAxisSensor(handles[2 + i]));
            }
        }
        add(4, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft);
        add(5, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight);
        configured = true;
        configured_style = style;
        configured_attributes = attributes;
    }

    ~SwitchSixAxis() {
        reset();
    }

    void reset() {
        for (size_t i = 0; i < handles.size(); ++i)
            if (active[i]) {
                hidStopSixAxisSensor(handles[i]);
                active[i] = false;
            }
        configured = false;
    }

    void add(size_t index, HidNpadIdType id, HidNpadStyleTag style) {
        if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&handles[index], 1, id, style)))
            active[index] = R_SUCCEEDED(hidStartSixAxisSensor(handles[index]));
    }

    void poll(EmuEnvState &emuenv, const PadState &pad) {
        if (emuenv.cfg.disable_motion || !padIsConnected(&pad)) {
            if (configured)
                reset();
            emuenv.ctrl.has_motion_support = false;
            return;
        }

        const u32 style = padGetStyleSet(&pad);
        const u32 attributes = padGetAttributes(&pad);
        if (!configured || style != configured_style || attributes != configured_attributes) {
            reset();
            acquire(style, attributes);
        }
        int handle_index = -1;
        bool right_joycon = false;
        if (style & HidNpadStyleTag_NpadHandheld)
            handle_index = 0;
        else if (style & HidNpadStyleTag_NpadFullKey)
            handle_index = 1;
        else if (style & HidNpadStyleTag_NpadJoyDual) {
            if (attributes & HidNpadAttribute_IsLeftConnected)
                handle_index = 2;
            else if (attributes & HidNpadAttribute_IsRightConnected) {
                handle_index = 3;
                right_joycon = true;
            }
        } else if (style & HidNpadStyleTag_NpadJoyLeft) {
            handle_index = 4;
        } else if (style & HidNpadStyleTag_NpadJoyRight) {
            handle_index = 5;
            right_joycon = true;
        }

        if (handle_index < 0 || !active[handle_index]) {
            // A controller can be connected after startup, while the initial
            // handle acquisition failed. Retry on the next frame.
            reset();
            emuenv.ctrl.has_motion_support = false;
            return;
        }
        emuenv.ctrl.has_motion_support = true;
        if (!emuenv.motion.is_sampling)
            return;

        HidSixAxisSensorState sensor{};
        if (hidGetSixAxisSensorStates(handles[handle_index], &sensor, 1) == 0
            || !(sensor.attributes & HidSixAxisSensorAttribute_IsConnected)) {
            reset();
            emuenv.ctrl.has_motion_support = false;
            return;
        }

        // Convert libnx axes (right, down, into screen) to SDL axes (right, up, out).
        // Handheld input also cancels Vita3K's gamepad rotation to match the console.
        // Convert acceleration from g to m/s^2 and angular velocity from rev/s to rad/s.
        const bool handheld = handle_index == 0;
        const auto send = [&](int type, const HidVector &source, float scale) {
            SDL_GamepadSensorEvent event{};
            event.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
            event.sensor_timestamp = SDL_GetTicksNS();
            event.data[0] = source.x * scale;
            event.data[1] = (handheld ? -source.z : -source.y) * scale;
            event.data[2] = (handheld ? source.y : -source.z) * scale;
            if (right_joycon) {
                event.data[0] = -event.data[0];
                event.data[1] = -event.data[1];
            }
            handle_motion_event(emuenv, type, event);
        };
        send(SDL_SENSOR_ACCEL, sensor.acceleration, SDL_STANDARD_GRAVITY);
        constexpr float radians_per_revolution = 6.28318530717958647692f;
        send(SDL_SENSOR_GYRO, sensor.angular_velocity, radians_per_revolution);
    }
};

struct SwitchControllerFrame {
    u64 held = 0;
    u64 down = 0;
    bool connected = false;
};

struct SwitchControllerStatus {
    bool initialized = false;
    bool connected = false;
    u32 style = 0;
};

// Buttons the pointer claims while it is on. The Vita has no mouse, so a game
// that needs one is navigating by touch: a click has to be a tap, and the same
// press must not also reach the pad.
constexpr u64 VIRTUAL_MOUSE_CLICK_BUTTONS =
    HidNpadButton_A | HidNpadButton_B | HidNpadButton_ZR;

static SwitchControllerFrame switch_poll_controller(EmuEnvState &emuenv, PadState &pad,
    SwitchSixAxis &six_axis, SwitchControllerStatus &status, bool intercepted,
    VirtualMouseState &vmouse) {
    padUpdate(&pad);
    SwitchControllerFrame frame{};
    frame.connected = padIsConnected(&pad);
    frame.held = frame.connected ? padGetButtons(&pad) : 0;
    frame.down = frame.connected ? padGetButtonsDown(&pad) : 0;
    const u32 style = frame.connected ? padGetStyleSet(&pad) : 0;

    if (!status.initialized || status.connected != frame.connected || status.style != style) {
        if (frame.connected)
            LOG_INFO("Switch controller connected (style=0x{:x}, attributes=0x{:x})", style, padGetAttributes(&pad));
        else
            LOG_INFO("Switch controller disconnected");
        if (!frame.connected)
            switch_stop_vibration();
        status.initialized = true;
        status.connected = frame.connected;
        status.style = style;
    }

    if (!frame.connected || intercepted) {
        vmouse.pressed = false;
        if (vmouse.cursor)
            vmouse.cursor->set_pressed(false);
        clear_switch_controller_input(emuenv);
        emuenv.touch.touchscreen_port = SCE_TOUCH_PORT_FRONT;
        emuenv.touch.touchscreen_both = false;
        std::fill_n(emuenv.touch.rear_touch_held, 4, false);
        if (!frame.connected)
            emuenv.ctrl.has_motion_support = false;
        return frame;
    }

    six_axis.poll(emuenv, pad);
    u64 held = frame.held;

    if (vmouse.enabled) {
        // Full deflection crosses the screen in about a second and a half, which
        // is quick enough to be useful and slow enough to hit a menu entry.
        constexpr float pointer_speed = 0.7f;
        constexpr float pointer_deadzone = 0.15f;
        const HidAnalogStickState stick = padGetStickPos(&pad, 0);
        float sx = static_cast<float>(stick.x) / 32767.0f;
        float sy = static_cast<float>(stick.y) / 32767.0f;
        const float magnitude = std::sqrt(sx * sx + sy * sy);
        if (magnitude > pointer_deadzone) {
            // Rescale past the deadzone so the first movement is not a jump.
            const float scale = (magnitude - pointer_deadzone) / (1.0f - pointer_deadzone) / magnitude;
            const float step = pointer_speed / 60.0f;
            vmouse.x = std::clamp(vmouse.x + sx * scale * step, 0.0f, 1.0f);
            // The stick's Y grows upwards, the screen's downwards.
            vmouse.y = std::clamp(vmouse.y - sy * scale * step, 0.0f, 1.0f);
        }
        vmouse.pressed = (held & VIRTUAL_MOUSE_CLICK_BUTTONS) != 0;
        if (vmouse.cursor) {
            vmouse.cursor->set_position(vmouse.x, vmouse.y);
            vmouse.cursor->set_pressed(vmouse.pressed);
        }
        // The claimed buttons never reach the mapping below, and the left stick
        // is zeroed after it so the guest sees a centred stick.
        held &= ~VIRTUAL_MOUSE_CLICK_BUTTONS;
    }

    uint32_t buttons = 0;
    uint32_t buttons_ext = 0;

    const auto set_common = [&](const u64 nx_button, const uint32_t vita_button) {
        if ((held & nx_button) != 0) {
            buttons |= vita_button;
            buttons_ext |= vita_button;
        }
    };
    const auto set_base = [&](const u64 nx_button, const uint32_t vita_button) {
        if ((held & nx_button) != 0)
            buttons |= vita_button;
    };
    const auto set_ext = [&](const u64 nx_button, const uint32_t vita_button) {
        if ((held & nx_button) != 0)
            buttons_ext |= vita_button;
    };

    set_common(HidNpadButton_A, vita_face_button(emuenv.cfg.switch_button_a, SCE_CTRL_CIRCLE));
    set_common(HidNpadButton_B, vita_face_button(emuenv.cfg.switch_button_b, SCE_CTRL_CROSS));
    set_common(HidNpadButton_X, vita_face_button(emuenv.cfg.switch_button_x, SCE_CTRL_TRIANGLE));
    set_common(HidNpadButton_Y, vita_face_button(emuenv.cfg.switch_button_y, SCE_CTRL_SQUARE));
    set_common(HidNpadButton_Plus, SCE_CTRL_START);
    set_common(HidNpadButton_Minus, SCE_CTRL_SELECT);
    set_common(HidNpadButton_Up, SCE_CTRL_UP);
    set_common(HidNpadButton_Right, SCE_CTRL_RIGHT);
    set_common(HidNpadButton_Down, SCE_CTRL_DOWN);
    set_common(HidNpadButton_Left, SCE_CTRL_LEFT);

    // The original Vita API exposes L/R; the extended API distinguishes
    // L1/R1, triggers, and stick clicks.
    set_base(HidNpadButton_L, SCE_CTRL_L);
    set_base(HidNpadButton_R, SCE_CTRL_R);
    set_ext(HidNpadButton_L, SCE_CTRL_L1);
    set_ext(HidNpadButton_R, SCE_CTRL_R1);
    // A handheld Vita has a rear panel and no L2/R2/L3/R3; a PS TV has those
    // four buttons and no rear panel. The mapping follows the emulated hardware:
    // on PS TV the buttons go to the pad, otherwise they are rear-panel
    // quadrants and claim those bits whole so no press is delivered twice.
    const bool rear_triggers = emuenv.cfg.switch_rear_touch_triggers
        && !emuenv.cfg.current_config.pstv_mode;
    emuenv.touch.rear_touch_held[0] = rear_triggers && (held & HidNpadButton_ZL);
    emuenv.touch.rear_touch_held[1] = rear_triggers && (held & HidNpadButton_ZR);
    emuenv.touch.rear_touch_held[2] = rear_triggers && (held & HidNpadButton_StickL);
    emuenv.touch.rear_touch_held[3] = rear_triggers && (held & HidNpadButton_StickR);

    const bool rear_zl = !rear_triggers && emuenv.cfg.switch_rear_touch == "zl" && (held & HidNpadButton_ZL);
    const bool rear_zr = !rear_triggers && emuenv.cfg.switch_rear_touch == "zr" && (held & HidNpadButton_ZR);
    emuenv.touch.touchscreen_port = (rear_zl || rear_zr) && !vmouse.enabled
        ? SCE_TOUCH_PORT_BACK
        : SCE_TOUCH_PORT_FRONT;
    emuenv.touch.touchscreen_both = !rear_triggers && emuenv.cfg.switch_rear_touch == "both" && !vmouse.enabled;
    if (!rear_triggers && emuenv.cfg.switch_rear_touch != "zl")
        set_ext(HidNpadButton_ZL, SCE_CTRL_L2);
    if (!rear_triggers && emuenv.cfg.switch_rear_touch != "zr")
        set_ext(HidNpadButton_ZR, SCE_CTRL_R2);
    if (!rear_triggers) {
        set_ext(HidNpadButton_StickL, SCE_CTRL_L3);
        set_ext(HidNpadButton_StickR, SCE_CTRL_R3);
    }

    HidAnalogStickState left = padGetStickPos(&pad, 0);
    const HidAnalogStickState right = padGetStickPos(&pad, 1);
    if (vmouse.enabled)
        left = HidAnalogStickState{};

    std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    auto &keyboard = emuenv.ctrl.keyboard_state;
    keyboard.buttons = buttons;
    keyboard.buttons_ext = buttons_ext;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    normalize_stick(left.x, left.y, emuenv.cfg.switch_stick_deadzone,
        emuenv.cfg.controller_analog_multiplier, lx, ly);
    normalize_stick(right.x, right.y, emuenv.cfg.switch_stick_deadzone,
        emuenv.cfg.controller_analog_multiplier, rx, ry);
    keyboard.axes[0] = lx;
    keyboard.axes[1] = -ly;
    keyboard.axes[2] = rx;
    keyboard.axes[3] = -ry;
    return frame;
}

enum class SwitchFrontendAction {
    None,
    ReturnToLauncher,
    ExitApplication,
};

struct SwitchRunResult {
    int exit_code = 0;
    SwitchFrontendAction action = SwitchFrontendAction::None;
    std::string title_id;
};

constexpr const char *SWITCH_FRONTEND_REQUEST_PATH = "sdmc:/switch/vita3k/.frontend_request";
constexpr const char *SWITCH_FRONTEND_REQUEST_TMP_PATH = "sdmc:/switch/vita3k/.frontend_request.tmp";
// Must match EXIT_DETECTION_STR in switch-launcher/fwd/main.c, which is the
// hbloader the forwarder actually runs. It is not the stock nx-hbloader string:
// with the wrong one the check at fwd/main.c falls through to loading the NRO,
// and svcSetProcessMemoryPermission then fails with 0xD401 because Vita3K has
// already exercised JIT and alias mappings in this process.
constexpr const char *HBLOADER_EXIT_SENTINEL = "__VITA3K_FORWARDER_EXIT__";

// A HOME-menu forwarder starts an nx-hbloader inside its own Application
// process. Reusing that process for the launcher after Vita3K has exercised JIT
// and alias mappings can leave hbloader unable to turn the launcher's mapped
// pages executable (KernelError_InvalidMemoryState / 0xD401). Verify that the
// current installed program really targets our return path so we can ask Horizon
// to restart the whole Application instead. That gives the launcher a pristine
// address space and does not involve an in-process NRO transition at all.
static bool current_forwarder_targets(const std::string &return_path) {
    if (return_path.empty())
        return false;

    u64 program_id = 0;
    const Result info_rc = svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(info_rc) || program_id == 0) {
        LOG_WARN("Could not identify the current Switch program (rc={}); clean launcher restart unavailable", log_hex(info_rc));
        return false;
    }

    char config_path[128]{};
    std::snprintf(config_path, sizeof(config_path),
        "sdmc:/switch/vita3k/forwarders/%016llx.cfg",
        static_cast<unsigned long long>(program_id));

    FILE *config = std::fopen(config_path, "rb");
    if (!config) {
        // Older/static NRO forwarders embed their target in ExeFS instead of
        // using our newer SD-side .cfg file. Vita3K forwarders occupy the 0x05
        // homebrew application-id range (retail applications use 0x01), so this
        // remains safe while avoiding a restart of an arbitrary title used for
        // hbmenu application override.
        if ((program_id >> 56) == 0x05) {
            LOG_INFO("Configless installed homebrew forwarder {:016x}; clean application restart allowed", program_id);
            return true;
        }
        LOG_WARN("Current program {:016x} has no Vita3K forwarder config and is not a 0x05 homebrew forwarder; clean launcher restart unavailable",
            program_id);
        return false;
    }

    char configured_target[FS_MAX_PATH]{};
    const size_t bytes_read = std::fread(configured_target, 1, sizeof(configured_target) - 1, config);
    std::fclose(config);
    configured_target[bytes_read] = '\0';

    if (configured_target[0] == '\0' || return_path != configured_target) {
        LOG_WARN("Current forwarder target '{}' does not match return path '{}'", configured_target, return_path);
        return false;
    }
    return true;
}

// The fresh launcher cannot receive the old NRO's argv because the complete
// Application is restarted. Persist only the one-shot frontend intent, using a
// temporary file + rename so a power loss cannot expose a partial request.
static bool write_frontend_request(SwitchFrontendAction action, const std::string &title_id) {
    FILE *request = std::fopen(SWITCH_FRONTEND_REQUEST_TMP_PATH, "wb");
    if (!request) {
        LOG_ERROR("Could not create the launcher return request");
        return false;
    }

    const char *request_action = "launcher";
    const int written = std::fprintf(request, "VITA3K_FRONTEND_REQUEST_V1\n%s\n%s\n",
        request_action, title_id.c_str());
    const bool closed = std::fclose(request) == 0;
    if (written < 0 || !closed) {
        std::remove(SWITCH_FRONTEND_REQUEST_TMP_PATH);
        LOG_ERROR("Could not write the launcher return request");
        return false;
    }

    std::remove(SWITCH_FRONTEND_REQUEST_PATH);
    if (std::rename(SWITCH_FRONTEND_REQUEST_TMP_PATH, SWITCH_FRONTEND_REQUEST_PATH) != 0) {
        std::remove(SWITCH_FRONTEND_REQUEST_TMP_PATH);
        LOG_ERROR("Could not publish the launcher return request");
        return false;
    }
    return true;
}

struct SwitchLifecycleState {
    std::atomic<int> focus_state{ -1 };
    std::atomic<bool> operation_mode_changed{ false };
    std::atomic<bool> performance_mode_changed{ false };
    std::atomic<bool> exit_requested{ false };
};

static void switch_applet_hook(AppletHookType hook, void *userdata) {
    auto &state = *static_cast<SwitchLifecycleState *>(userdata);
    switch (hook) {
    case AppletHookType_OnFocusState:
        state.focus_state.store(static_cast<int>(appletGetFocusState()), std::memory_order_release);
        break;
    case AppletHookType_OnResume:
        state.focus_state.store(static_cast<int>(AppletFocusState_InFocus), std::memory_order_release);
        break;
    case AppletHookType_OnOperationMode:
        state.operation_mode_changed.store(true, std::memory_order_release);
        break;
    case AppletHookType_OnPerformanceMode:
        state.performance_mode_changed.store(true, std::memory_order_release);
        break;
    case AppletHookType_OnExitRequest:
        state.exit_requested.store(true, std::memory_order_release);
        break;
    default:
        break;
    }
}

class ScopedSwitchAppletHook {
public:
    explicit ScopedSwitchAppletHook(SwitchLifecycleState &state) {
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
        appletHook(&cookie, switch_applet_hook, &state);
        state.focus_state.store(static_cast<int>(appletGetFocusState()), std::memory_order_release);
    }

    ~ScopedSwitchAppletHook() {
        appletUnhook(&cookie);
    }

private:
    AppletHookCookie cookie{};
};

// ---------------------------------------------------------------------------
// Quick-menu sub-screens
// ---------------------------------------------------------------------------

// Entries of the top-level quick menu, in the order pause_overlay draws them.
enum QuickMenuEntry {
    QUICK_MENU_RESUME = 0,
    QUICK_MENU_FRAME_GEN,
    QUICK_MENU_VIRTUAL_MOUSE,
    QUICK_MENU_TROPHIES,
    QUICK_MENU_SETTINGS,
    QUICK_MENU_LAUNCHER,
    QUICK_MENU_EXIT,
};
static_assert(overlay::pause_overlay::k_menu_entries == QUICK_MENU_EXIT + 1,
    "quick menu entry list and pause_overlay label count disagree");

enum class QuickMenuScreen {
    Menu,
    TrophyGroups,
    TrophyList,
    Settings,
};

struct SwitchTrophyEntry {
    std::string name;
    std::string detail;
    std::string icon_path;
    int grade = 0;
    bool hidden = false;
    bool earned = false;
    uint64_t timestamp = 0;
};

struct SwitchTrophyGroup {
    std::string title;
    std::string icon_path;
    int total = 0;
    int unlocked = 0;
    std::vector<SwitchTrophyEntry> trophies;
};

static const char *trophy_grade_name(int grade) {
    // SceNpTrophyGrade, np/trophy/context.h.
    switch (grade) {
    case 1: return "Platinum";
    case 2: return "Gold";
    case 3: return "Silver";
    case 4: return "Bronze";
    default: return "Trophy";
    }
}

static std::string format_unlock_time(uint64_t unix_seconds) {
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm parts{};
#ifdef _WIN32
    if (localtime_s(&parts, &value) != 0)
        return {};
#else
    if (!localtime_r(&value, &parts))
        return {};
#endif
    char buffer[32];
    if (!std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts))
        return {};
    return buffer;
}

// Reads the active user's trophy tree. Only safe while the session is paused:
// load_collection goes through the guest IO layer, whose descriptor table is not
// synchronised against running guest threads.
static std::vector<SwitchTrophyGroup> load_switch_trophies(EmuEnvState &emuenv) {
    std::vector<SwitchTrophyGroup> groups;

    np::trophy::CollectionSource source;
    source.io = &emuenv.io;
    source.vita_fs_path = emuenv.vita_fs_path;
    source.user_id = emuenv.io.user_id;
    source.lang = static_cast<uint32_t>(emuenv.cfg.sys_lang);

    for (const auto &np_com_id : np::trophy::list_collection_ids(source)) {
        np::trophy::CollectionSnapshot snapshot;
        if (!np::trophy::load_collection(source, np_com_id, snapshot))
            continue;

        SwitchTrophyGroup group;
        group.title = snapshot.title.empty() ? np_com_id : snapshot.title;
        group.icon_path = snapshot.icon_path;
        group.total = snapshot.total;
        group.unlocked = snapshot.unlocked;
        group.trophies.reserve(snapshot.trophies.size());
        for (const auto &record : snapshot.trophies) {
            SwitchTrophyEntry entry;
            entry.name = record.name;
            entry.detail = record.detail;
            entry.icon_path = record.icon_path;
            entry.grade = record.grade;
            entry.hidden = record.hidden;
            entry.earned = record.earned;
            entry.timestamp = record.timestamp;
            group.trophies.push_back(std::move(entry));
        }
        groups.push_back(std::move(group));
    }

    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
        return a.title < b.title;
    });
    return groups;
}

static std::vector<overlay::list_row> build_trophy_group_rows(
    const std::vector<SwitchTrophyGroup> &groups) {
    std::vector<overlay::list_row> rows;
    rows.reserve(groups.size());
    for (const auto &group : groups) {
        const int percent = group.total > 0 ? group.unlocked * 100 / group.total : 0;
        overlay::list_row row;
        row.primary = group.title;
        row.secondary = fmt::format("{} of {} trophies", group.unlocked, group.total);
        row.value = fmt::format("{}%", percent);
        row.icon_path = group.icon_path;
        rows.push_back(std::move(row));
    }
    return rows;
}

static std::vector<overlay::list_row> build_trophy_rows(const SwitchTrophyGroup &group) {
    std::vector<overlay::list_row> rows;
    rows.reserve(group.trophies.size());
    for (const auto &trophy : group.trophies) {
        // A hidden trophy stays hidden until it is earned, matching the Vita.
        const bool conceal = trophy.hidden && !trophy.earned;
        overlay::list_row row;
        row.primary = conceal ? "Hidden trophy" : trophy.name;
        row.secondary = conceal ? "" : trophy.detail;
        row.icon_path = trophy.earned ? trophy.icon_path : std::string();
        row.dimmed = !trophy.earned;
        row.value = trophy.earned
            ? (trophy.timestamp ? format_unlock_time(trophy.timestamp) : std::string("Earned"))
            : std::string(trophy_grade_name(trophy.grade));
        rows.push_back(std::move(row));
    }
    return rows;
}

// Settings that app::apply_runtime_settings() genuinely re-applies to a running
// session. resolution-multiplier is absent: it is on Vita3K's restart-required
// list and is reverted in the live config. Changes are transient - the launcher
// owns config.yml and rewrites it on every launch.
enum LiveSetting {
    LIVE_SCREEN_FILTER = 0,
    LIVE_VSYNC,
    LIVE_STRETCH,
    LIVE_FPS_HACK,
    LIVE_SURFACE_SYNC,
    LIVE_AUDIO_VOLUME,
    LIVE_PERF_OVERLAY,
    LIVE_PERF_DETAIL,
    LIVE_PERF_POSITION,
    LIVE_SETTING_COUNT,
};

static std::vector<const char *> supported_screen_filters(EmuEnvState &emuenv) {
    std::vector<const char *> filters;
    if (!emuenv.renderer)
        return { "Bilinear" };
    const int mask = emuenv.renderer->get_supported_filters();
    if (mask & static_cast<int>(renderer::Filter::NEAREST))
        filters.push_back("Nearest");
    if (mask & static_cast<int>(renderer::Filter::BILINEAR))
        filters.push_back("Bilinear");
    if (mask & static_cast<int>(renderer::Filter::BICUBIC))
        filters.push_back("Bicubic");
    if (mask & static_cast<int>(renderer::Filter::FXAA))
        filters.push_back("FXAA");
    if (mask & static_cast<int>(renderer::Filter::FSR))
        filters.push_back("FSR");
    if (filters.empty())
        filters.push_back("Bilinear");
    return filters;
}

static const char *on_off(bool value) {
    return value ? "On" : "Off";
}

static std::vector<overlay::list_row> build_live_setting_rows(EmuEnvState &emuenv) {
    static constexpr std::array<const char *, 4> detail_names = {
        "Minimum", "Low", "Medium", "Maximum"
    };
    static constexpr std::array<const char *, 6> position_names = {
        "Top left", "Top center", "Top right",
        "Bottom left", "Bottom center", "Bottom right"
    };

    const auto &cc = emuenv.cfg.current_config;
    std::vector<overlay::list_row> rows(LIVE_SETTING_COUNT);

    rows[LIVE_SCREEN_FILTER].primary = "Screen filter";
    rows[LIVE_SCREEN_FILTER].value = cc.screen_filter.empty() ? "None" : cc.screen_filter;
    rows[LIVE_VSYNC].primary = "VSync";
    rows[LIVE_VSYNC].value = on_off(cc.v_sync);
    rows[LIVE_STRETCH].primary = "Stretch to screen";
    rows[LIVE_STRETCH].value = on_off(cc.stretch_the_display_area);
    rows[LIVE_FPS_HACK].primary = "FPS hack";
    rows[LIVE_FPS_HACK].value = on_off(cc.fps_hack);
    rows[LIVE_SURFACE_SYNC].primary = "Disable surface sync";
    rows[LIVE_SURFACE_SYNC].value = on_off(cc.disable_surface_sync);
    rows[LIVE_AUDIO_VOLUME].primary = "Audio volume";
    rows[LIVE_AUDIO_VOLUME].value = fmt::format("{}%", cc.audio_volume);
    rows[LIVE_PERF_OVERLAY].primary = "Performance overlay";
    rows[LIVE_PERF_OVERLAY].value = on_off(emuenv.cfg.performance_overlay);

    const int detail = std::clamp(emuenv.cfg.performance_overlay_detail, 0,
        static_cast<int>(detail_names.size()) - 1);
    rows[LIVE_PERF_DETAIL].primary = "Overlay detail";
    rows[LIVE_PERF_DETAIL].value = detail_names[static_cast<size_t>(detail)];
    rows[LIVE_PERF_DETAIL].dimmed = !emuenv.cfg.performance_overlay;

    const int position = std::clamp(emuenv.cfg.performance_overlay_position, 0,
        static_cast<int>(position_names.size()) - 1);
    rows[LIVE_PERF_POSITION].primary = "Overlay position";
    rows[LIVE_PERF_POSITION].value = position_names[static_cast<size_t>(position)];
    rows[LIVE_PERF_POSITION].dimmed = !emuenv.cfg.performance_overlay;

    return rows;
}

// Applies exactly the field that changed, rather than going through
// app::apply_runtime_settings(): that re-applies the locale and the texture
// replacement state too, neither of which this menu touches, and both of which
// are read by the render thread. Returns false for a row that is not adjustable.
static bool adjust_live_setting(EmuEnvState &emuenv, int index, int direction) {
    auto &cc = emuenv.cfg.current_config;
    auto *renderer = emuenv.renderer.get();
    const auto cycle = [direction](int value, int count) {
        return (value + direction + count) % count;
    };

    switch (index) {
    case LIVE_SCREEN_FILTER: {
        const auto filters = supported_screen_filters(emuenv);
        int current = 0;
        for (size_t i = 0; i < filters.size(); ++i) {
            if (cc.screen_filter == filters[i]) {
                current = static_cast<int>(i);
                break;
            }
        }
        cc.screen_filter = filters[static_cast<size_t>(cycle(current, static_cast<int>(filters.size())))];
        if (renderer)
            renderer->set_screen_filter(cc.screen_filter);
        break;
    }
    case LIVE_VSYNC:
        cc.v_sync = !cc.v_sync;
        if (renderer)
            renderer->set_vsync_state(cc.v_sync);
        break;
    case LIVE_STRETCH:
        cc.stretch_the_display_area = !cc.stretch_the_display_area;
        if (renderer)
            renderer->set_stretch_display(cc.stretch_the_display_area);
        break;
    case LIVE_FPS_HACK:
        cc.fps_hack = !cc.fps_hack;
        emuenv.display.fps_hack = cc.fps_hack;
        break;
    case LIVE_SURFACE_SYNC:
        cc.disable_surface_sync = !cc.disable_surface_sync;
        if (renderer)
            renderer->set_surface_sync_state(cc.disable_surface_sync);
        break;
    case LIVE_AUDIO_VOLUME:
        cc.audio_volume = std::clamp(cc.audio_volume + direction * 5, 0, 100);
        emuenv.audio.set_global_volume(cc.audio_volume / 100.f);
        break;
    case LIVE_PERF_OVERLAY:
        emuenv.cfg.performance_overlay = !emuenv.cfg.performance_overlay;
        app::sync_perf_overlay_config(emuenv);
        break;
    case LIVE_PERF_DETAIL:
        if (!emuenv.cfg.performance_overlay)
            return false;
        emuenv.cfg.performance_overlay_detail = cycle(emuenv.cfg.performance_overlay_detail, 4);
        app::sync_perf_overlay_config(emuenv);
        break;
    case LIVE_PERF_POSITION:
        if (!emuenv.cfg.performance_overlay)
            return false;
        emuenv.cfg.performance_overlay_position = cycle(emuenv.cfg.performance_overlay_position, 6);
        app::sync_perf_overlay_config(emuenv);
        break;
    default:
        return false;
    }

    return true;
}

static std::shared_ptr<overlay::pause_overlay> show_switch_quick_menu(EmuEnvState &emuenv,
    app::AppSessionController &controller, SwitchTouchTracker &touch_tracker, int selection,
    bool virtual_mouse) {
    if (!emuenv.renderer || !emuenv.renderer->overlay_manager)
        return {};
    controller.set_pause_reason(app::AppSessionPauseReason::Menu, true);

    // Created once per session and kept: remove() defers to a type-keyed queue
    // when the list is busy, and a deferred removal applied after the next
    // create would take the new overlay with it.
    auto *manager = emuenv.renderer->overlay_manager;
    auto menu = manager->get<overlay::pause_overlay>();
    if (!menu)
        menu = manager->create<overlay::pause_overlay>();
    menu->visible = true;
    const bool lsfg_available = emuenv.backend_renderer == renderer::Backend::Vulkan
        && renderer::vulkan::lsfg::is_available();
    menu->set_switch_menu(true, selection,
        lsfg_available,
        lsfg_available && renderer::vulkan::lsfg::is_enabled(),
        virtual_mouse);
    clear_switch_controller_input(emuenv);
    emuenv.touch.finger_count = 0;
    touch_tracker.reset();
    // The guest is suspended, so no further command list will arrive; ask for a
    // present so the render thread leaves its wait and draws the menu.
    emuenv.renderer->async_flip_requested.store(true, std::memory_order_release);
    return menu;
}

static void hide_switch_quick_menu(app::AppSessionController &controller,
    const std::shared_ptr<overlay::pause_overlay> &menu) {
    if (menu) {
        menu->set_switch_menu(false);
        menu->visible = false;
        menu->refresh();
    }
    controller.set_pause_reason(app::AppSessionPauseReason::Menu, false);
    if (auto *emuenv = get_emuenv(); emuenv && emuenv->renderer)
        emuenv->renderer->async_flip_requested.store(true, std::memory_order_release);
}

// Per-game boot + event loop. Mirrors main_android.cpp's SDL_main body without
// the JNI/IME plumbing.
SwitchRunResult run_game(const std::string &title_id, NWindow *nwindow) {
    SwitchRunResult result{};
    result.title_id = title_id;
    // After the last consoleExit, which resets the stderr device libnx owns.
    switch_capture_stderr();
    auto *emuenv = get_emuenv();
    auto *session_controller = get_app_session_controller();
    if (!emuenv || !session_controller) {
        LOG_ERROR("Emulator not initialized");
        result.exit_code = -1;
        return result;
    }

    AppLaunchRequest launch_request{
        .app_path = title_id,
    };
    bool relaunch_requested = false;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    SwitchSixAxis six_axis;
    SwitchLifecycleState lifecycle;
    ScopedSwitchAppletHook applet_hook(lifecycle);

    do {
        relaunch_requested = false;
        SwitchFrameHost frame_host(nwindow);
        std::optional<AppLaunchRequest> pending_launch_request;

        const auto cleanup_launch = [&](const app::AppSessionStopReason reason) {
            // Teardown was the one unlogged stretch of the exit path, so a hang in
            // it produced a log that simply stopped. These are warn level and set a
            // crash breadcrumb, so they survive an unflushed buffer.
            logging::set_crash_breadcrumb("stopping the session");
            LOG_WARN("[shutdown] stopping the session");
            // Every Vulkan call fails once the device is lost, teardown
            // included. Keep unwinding: taking the process down here would lose
            // the return to the launcher.
            try {
                session_controller->stop(reason);
            } catch (const std::exception &error) {
                LOG_ERROR("[shutdown] session teardown failed: {}", error.what());
            }
            logging::set_crash_breadcrumb("shutting down SDL");
            LOG_WARN("[shutdown] shutting down SDL");
            SDL_Quit();
            logging::set_crash_breadcrumb("session torn down");
            LOG_WARN("[shutdown] session torn down");
        };


        // Video is provided directly through libnx Vulkan or EGL, audio by the selected native backend,
        // and controller/touch input by libnx. SDL still owns the event queue and
        // main-loop timing used by the platform-agnostic frontend.
        if (!SDL_Init(SDL_INIT_EVENTS)) {
            LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        LOG_INFO("Switch controller input initialized through libnx HID");

        if (!session_controller->begin_launch(launch_request, launch_request.reason != AppLaunchReason::LoadExec)) {
            LOG_ERROR("Could not find app '{}' in apps list.", launch_request.app_path);
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        if (emuenv->backend_renderer == renderer::Backend::Vulkan && !emuenv->vulkan_device_info)
            emuenv->vulkan_device_info = std::make_unique<renderer::VulkanDeviceInfo>(renderer::enumerate_vulkan_devices());

        if (!frame_host.initialize_for_backend(
                emuenv->backend_renderer, emuenv->cfg.current_config.backend_renderer)) {
            LOG_ERROR("Failed to initialize the selected Switch graphics backend '{}'.",
                emuenv->cfg.current_config.backend_renderer);
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        if (!session_controller->initialize_renderer(frame_host)) {
            LOG_ERROR("Failed to initialise renderer.");
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        if (!session_controller->initialize_runtime()) {
            LOG_ERROR("Failed late initialisation.");
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        if (!session_controller->load_and_run()) {
            LOG_ERROR("Failed to load or start the app session.");
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            result.exit_code = -1;
            return result;
        }

        if (auto request = emuenv->take_app_launch_request())
            pending_launch_request = std::move(request);

        LOG_INFO("Game started: {} ({})", emuenv->current_app_title, launch_request.app_path);
        app::LaunchRuntimeMetrics runtime_metrics{};
        // Sampled while running so a steady climb is distinguishable from a
        // single oversized allocation when the host heap runs out.

        bool running = !pending_launch_request.has_value();
        bool quick_menu_open = false;
        int quick_menu_selection = 0;
        QuickMenuScreen quick_menu_screen = QuickMenuScreen::Menu;
        int quick_menu_list_selection = 0;
        int trophy_group_selection = 0;
        int menu_repeat_frames = 0;
        std::vector<SwitchTrophyGroup> trophy_groups;
        std::shared_ptr<overlay::pause_overlay> quick_menu_overlay;
        SwitchControllerStatus controller_status{};
        SwitchTouchTracker touch_tracker{};
        VirtualMouseState virtual_mouse{};
        // The cursor overlay exists only while the pointer is on, so turning it
        // off leaves nothing of it behind on screen or in the overlay list.
        const auto set_virtual_mouse = [&](bool enabled) {
            virtual_mouse.enabled = enabled;
            virtual_mouse.pressed = false;
            auto *manager = emuenv->renderer ? emuenv->renderer->overlay_manager : nullptr;
            if (!manager)
                return;
            if (enabled) {
                virtual_mouse.cursor = manager->create<VirtualMouseCursor>();
                virtual_mouse.cursor->set_position(virtual_mouse.x, virtual_mouse.y);
            } else {
                manager->remove<VirtualMouseCursor>();
                virtual_mouse.cursor.reset();
            }
        };
        switch_start_preemption_watchdog(*emuenv);

        // Guest process time must not advance while the session is stopped:
        // sceKernelGetProcessTime is a raw wall-clock delta from start_tick, so
        // a 30 second HOME visit would hand the guest a 30 second frame delta.
        uint64_t last_loop_tick = switch_guest_tick(*emuenv);
        bool session_was_paused = false;

        while (running) {
            if (!appletMainLoop()) {
                // appletMainLoop() returns false as soon as Horizon requests
                // application exit, which can happen before the hook flag is
                // consumed below. Never reinterpret that exit as a request to
                // return to the launcher.
                lifecycle.exit_requested.store(false, std::memory_order_release);
                LOG_INFO("Applet main loop requested a clean exit");
                result.action = SwitchFrontendAction::ExitApplication;
                break;
            }

            if (running && emuenv->renderer && emuenv->renderer->device_lost.load(std::memory_order_acquire)) {
                LOG_CRITICAL("Closing the session: the GPU device was lost.");
                result.action = SwitchFrontendAction::ExitApplication;
                running = false;
            }

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    result.action = SwitchFrontendAction::ExitApplication;
                    running = false;
                    break;
                default:
                    break;
                }
            }

            const int focus = lifecycle.focus_state.exchange(-1, std::memory_order_acq_rel);
            if (focus >= 0) {
                const bool background = focus != static_cast<int>(AppletFocusState_InFocus);
                session_controller->set_pause_reason(app::AppSessionPauseReason::Background, background);
                LOG_INFO("Applet focus changed: {}", background ? "background (paused)" : "foreground (resumed)");
            }

            {
                // The freeze can land on either side of the focus event, so the
                // gap is donated back whenever this iteration or the previous one
                // saw a stopped session. Guest threads are suspended throughout,
                // so nothing observes start_tick moving.
                const uint64_t loop_tick = switch_guest_tick(*emuenv);
                const bool session_paused = session_controller->is_paused();
                if ((session_was_paused || session_paused) && loop_tick > last_loop_tick)
                    switch_advance_guest_start_tick(*emuenv, loop_tick - last_loop_tick);
                last_loop_tick = loop_tick;
                session_was_paused = session_paused;
            }
            if (lifecycle.operation_mode_changed.exchange(false, std::memory_order_acq_rel)) {
                // VI rescales the unchanged NWindow to the new panel, so the
                // swapchain does not have to be rebuilt. Force a present so the
                // first frame on the new output is current.
                if (emuenv->renderer)
                    emuenv->renderer->async_flip_requested.store(true, std::memory_order_release);
            }
            if (lifecycle.performance_mode_changed.exchange(false, std::memory_order_acq_rel))
                LOG_INFO("Applet performance mode changed: {}", static_cast<int>(appletGetPerformanceMode()));
            if (lifecycle.exit_requested.exchange(false, std::memory_order_acq_rel)) {
                LOG_INFO("Applet requested a clean exit");
                result.action = SwitchFrontendAction::ExitApplication;
                running = false;
            }

            if (!running)
                break;

            if (!quick_menu_open && ime::has_pending_request()) {
                session_controller->set_pause_reason(app::AppSessionPauseReason::User, true);
                clear_switch_controller_input(*emuenv);
                ime::process_pending_request(*emuenv);
                session_controller->set_pause_reason(app::AppSessionPauseReason::User, false);
                lifecycle.focus_state.store(static_cast<int>(appletGetFocusState()), std::memory_order_release);
            }

            // The minimal SDL3 Switch backend doesn't expose either device.
            const bool input_intercepted = quick_menu_open || session_controller->is_paused();
            const SwitchControllerFrame controller = switch_poll_controller(
                *emuenv, pad, six_axis, controller_status, input_intercepted, virtual_mouse);

            const bool menu_chord = controller.connected
                && (controller.held & (HidNpadButton_L | HidNpadButton_R)) == (HidNpadButton_L | HidNpadButton_R)
                && (controller.down & HidNpadButton_Plus);
            if (menu_chord) {
                if (quick_menu_open) {
                    hide_switch_quick_menu(*session_controller, quick_menu_overlay);
                    quick_menu_overlay.reset();
                    quick_menu_open = false;
                    quick_menu_screen = QuickMenuScreen::Menu;
                    trophy_groups.clear();
                } else {
                    quick_menu_selection = 0;
                    quick_menu_screen = QuickMenuScreen::Menu;
                    menu_repeat_frames = 0;
                    quick_menu_overlay = show_switch_quick_menu(
                        *emuenv, *session_controller, touch_tracker, quick_menu_selection,
                        virtual_mouse.enabled);
                    quick_menu_open = static_cast<bool>(quick_menu_overlay);
                }
            } else if (quick_menu_open) {
                const bool lsfg_available = emuenv->backend_renderer == renderer::Backend::Vulkan
                    && renderer::vulkan::lsfg::is_available();
                const bool in_list = quick_menu_screen != QuickMenuScreen::Menu;
                const QuickMenuScreen entry_screen = quick_menu_screen;

                const auto list_count = [&]() -> int {
                    switch (quick_menu_screen) {
                    case QuickMenuScreen::TrophyGroups: return static_cast<int>(trophy_groups.size());
                    case QuickMenuScreen::TrophyList:
                        return trophy_group_selection < static_cast<int>(trophy_groups.size())
                            ? static_cast<int>(trophy_groups[static_cast<size_t>(trophy_group_selection)].trophies.size())
                            : 0;
                    case QuickMenuScreen::Settings: return LIVE_SETTING_COUNT;
                    default: return 0;
                    }
                }();

                const auto close_menu = [&] {
                    hide_switch_quick_menu(*session_controller, quick_menu_overlay);
                    quick_menu_overlay.reset();
                    quick_menu_open = false;
                    quick_menu_screen = QuickMenuScreen::Menu;
                    trophy_groups.clear();
                    menu_repeat_frames = 0;
                };
                const auto back_to_menu = [&] {
                    quick_menu_screen = QuickMenuScreen::Menu;
                    if (quick_menu_overlay)
                        quick_menu_overlay->close_list();
                };
                const auto refresh_settings_rows = [&] {
                    if (quick_menu_overlay)
                        quick_menu_overlay->set_list("Settings",
                            "A / Left / Right: change    B: back    Changes last until the game closes",
                            build_live_setting_rows(*emuenv), quick_menu_list_selection);
                };

                // Edge-triggered input plus a held-repeat, so a long trophy list
                // is not one press per row.
                const bool up_held = (controller.held & HidNpadButton_Up) != 0;
                const bool down_held = (controller.held & HidNpadButton_Down) != 0;
                int step = 0;
                if (controller.down & HidNpadButton_Up)
                    step = -1;
                else if (controller.down & HidNpadButton_Down)
                    step = 1;
                if (step != 0) {
                    menu_repeat_frames = 0;
                } else if (up_held || down_held) {
                    menu_repeat_frames++;
                    if (menu_repeat_frames > 24 && (menu_repeat_frames % 4) == 0)
                        step = up_held ? -1 : 1;
                } else {
                    menu_repeat_frames = 0;
                }

                // L / R page through a list a screenful at a time.
                int page = 0;
                if (in_list && (controller.down & HidNpadButton_L))
                    page = -overlay::pause_overlay::k_list_visible_rows;
                else if (in_list && (controller.down & HidNpadButton_R))
                    page = overlay::pause_overlay::k_list_visible_rows;

                if (!in_list) {
                    if (step != 0) {
                        do {
                            quick_menu_selection = (quick_menu_selection + step
                                                       + overlay::pause_overlay::k_menu_entries)
                                % overlay::pause_overlay::k_menu_entries;
                        } while (quick_menu_selection == QUICK_MENU_FRAME_GEN && !lsfg_available);
                        if (quick_menu_overlay)
                            quick_menu_overlay->set_switch_menu_selection(quick_menu_selection);
                    }
                } else if (list_count > 0 && (step != 0 || page != 0)) {
                    quick_menu_list_selection = std::clamp(
                        quick_menu_list_selection + step + page, 0, list_count - 1);
                    if (quick_menu_overlay)
                        quick_menu_overlay->set_list_selection(quick_menu_list_selection);
                }

                if (controller.down & HidNpadButton_B) {
                    switch (quick_menu_screen) {
                    case QuickMenuScreen::Menu:
                        close_menu();
                        break;
                    case QuickMenuScreen::TrophyList:
                        quick_menu_screen = QuickMenuScreen::TrophyGroups;
                        quick_menu_list_selection = trophy_group_selection;
                        if (quick_menu_overlay)
                            quick_menu_overlay->set_list("Trophies", "A: open    B: back",
                                build_trophy_group_rows(trophy_groups), quick_menu_list_selection);
                        break;
                    default:
                        back_to_menu();
                        break;
                    }
                } else if (controller.down & (HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Right)) {
                    const int direction = (controller.down & HidNpadButton_Left) ? -1 : 1;
                    const bool confirm = (controller.down & HidNpadButton_A) != 0;

                    switch (quick_menu_screen) {
                    case QuickMenuScreen::Menu:
                        if (!confirm)
                            break;
                        switch (quick_menu_selection) {
                        case QUICK_MENU_RESUME:
                            close_menu();
                            break;
                        case QUICK_MENU_FRAME_GEN: {
                            const bool next = !renderer::vulkan::lsfg::is_enabled();
                            if (renderer::vulkan::lsfg::request_enabled(next) && quick_menu_overlay)
                                quick_menu_overlay->set_switch_menu_lsfg(
                                    renderer::vulkan::lsfg::is_available(),
                                    renderer::vulkan::lsfg::is_enabled());
                            break;
                        }
                        case QUICK_MENU_VIRTUAL_MOUSE:
                            set_virtual_mouse(!virtual_mouse.enabled);
                            if (quick_menu_overlay)
                                quick_menu_overlay->set_switch_menu_virtual_mouse(
                                    virtual_mouse.enabled);
                            break;
                        case QUICK_MENU_TROPHIES:
                            // Guest threads are suspended by the Menu pause
                            // reason, so the guest IO layer is safe to touch.
                            trophy_groups = load_switch_trophies(*emuenv);
                            trophy_group_selection = 0;
                            quick_menu_list_selection = 0;
                            quick_menu_screen = QuickMenuScreen::TrophyGroups;
                            if (quick_menu_overlay)
                                quick_menu_overlay->set_list("Trophies", "A: open    B: back",
                                    build_trophy_group_rows(trophy_groups), 0);
                            break;
                        case QUICK_MENU_SETTINGS:
                            quick_menu_list_selection = 0;
                            quick_menu_screen = QuickMenuScreen::Settings;
                            refresh_settings_rows();
                            break;
                        default:
                            result.title_id = launch_request.app_path;
                            result.action = quick_menu_selection == QUICK_MENU_LAUNCHER
                                ? SwitchFrontendAction::ReturnToLauncher
                                : SwitchFrontendAction::ExitApplication;
                            running = false;
                            break;
                        }
                        break;

                    case QuickMenuScreen::TrophyGroups:
                        if (!confirm || trophy_groups.empty())
                            break;
                        trophy_group_selection = quick_menu_list_selection;
                        quick_menu_list_selection = 0;
                        quick_menu_screen = QuickMenuScreen::TrophyList;
                        if (quick_menu_overlay) {
                            const auto &group = trophy_groups[static_cast<size_t>(trophy_group_selection)];
                            quick_menu_overlay->set_list(group.title,
                                fmt::format("{} of {} earned    B: back", group.unlocked, group.total),
                                build_trophy_rows(group), 0);
                        }
                        break;

                    case QuickMenuScreen::TrophyList:
                        break;

                    case QuickMenuScreen::Settings:
                        adjust_live_setting(*emuenv, quick_menu_list_selection, direction);
                        refresh_settings_rows();
                        break;
                    }
                }

                // Starting a new screen must not inherit the previous one's
                // auto-repeat, or a held direction runs away down the new list.
                if (quick_menu_screen != entry_screen)
                    menu_repeat_frames = 0;
            }

            switch_poll_touch(*emuenv, touch_tracker,
                !quick_menu_open && !session_controller->is_paused(), virtual_mouse);

            if (!pending_launch_request) {
                if (auto request = emuenv->take_app_launch_request()) {
                    pending_launch_request = std::move(request);
                    running = false;
                }
            }

            app::update_runtime_metrics(*emuenv, runtime_metrics);

            if (!session_controller->is_running())
                running = false;

            if (running)
                SDL_Delay(16);
        }

        LOG_INFO("Shutting down game");
        switch_stop_preemption_watchdog();
        switch_stop_vibration();

        if (quick_menu_open) {
            hide_switch_quick_menu(*session_controller, quick_menu_overlay);
            quick_menu_overlay.reset();
            quick_menu_open = false;
        }

        if (pending_launch_request) {
            // A plain guest process exit is a terminal notification, not a
            // load-exec request. The Qt frontend makes the same distinction.
            if (pending_launch_request->reason == AppLaunchReason::ProcessExit) {
                LOG_INFO("Guest process exited; ending the current title without an in-process relaunch");
            } else {
                launch_request = std::move(*pending_launch_request);
                relaunch_requested = true;
            }
        }

        cleanup_launch(relaunch_requested
                ? app::AppSessionStopReason::Relaunch
                : app::AppSessionStopReason::UserRequest);
    } while (relaunch_requested);

    return result;
}

} // namespace

int main(int argc, char *argv[]) {
    // Keep everything off core 3 (Horizon reserves it for the OS: HID, fs, display,
    // scheduler). A title-takeover homebrew is otherwise allowed to use it, and any
    // busy thread there starves the system and hard-locks the console.
    switch_pin_to_app_cores("main thread");

    // libnx runtime: romfs (bundled static assets) + sockets (net/http).
    romfsInit();
    socketInitializeDefault();
    SDL_SetMainReady();

    const fs::path storage_path = fs::path("sdmc:/switch/vita3k/") / "";

    // Detect install mode up front: it takes a lightweight init path (no guest
    // memory pool, no Vulkan, no threads) so nothing heavyweight survives the
    // envSetNextLoad chainload back to the launcher.
    bool install_mode = false;
    std::string return_path;
    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "--install") {
            install_mode = true;
        } else if (argument == "--return" && i + 1 < argc) {
            return_path = argv[++i];
        }
    }

    auto &session = switch_session_state();
    if (!init_switch_session(storage_path, session.root_paths, session.emuenv, install_mode)) {
        // Fall back to an on-screen console so the failure is visible on device.
        consoleInit(nullptr);
        printf("Vita3K: initialisation failed. Check sdmc:/switch/vita3k/logs.\n");
        printf("Press + to exit.\n");
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        session.app_session_controller.reset();
        session.emuenv.reset();
        switch_release_guest_region();
        switch_restore_loader_heap();
        if (auto logger = spdlog::default_logger())
            logger->flush();
        socketExit();
        romfsExit();
        return 1;
    }

    // The session controller owns runtime (renderer/audio/kernel) state that only a
    // game needs - skip it in install mode, whose emuenv has no such subsystems.
    if (!install_mode)
        session.app_session_controller = std::make_unique<app::AppSessionController>(*session.emuenv);
    LOG_INFO("Vita3K Switch session initialised.");
    LOG_INFO("[boot] install_mode={} return='{}' nextload_available={} argc={}",
        install_mode, return_path, envHasNextLoad(), argc);

    // Install mode: process sdmc:/switch/vita3k/install/ then exit (used by the
    // launcher, which chainloads "Vita3K.nro --install --return <launcher.nro>").
    if (install_mode) {
        run_switch_installer(*session.emuenv);

        // Destroy the session before shutting down libnx services.
        session.app_session_controller.reset();
        session.emuenv.reset();

        // Defensive: install mode reserves no guest memory, but if any path ever
        // did, free it before the chainload so hbloader gets a clean address space.
        switch_release_guest_region();
        switch_restore_loader_heap();

        // Restart the application to prevent the next NRO from reusing the installer's process.
        bool application_relaunch_queued = false;
        if (current_forwarder_targets(return_path)
            && write_frontend_request(SwitchFrontendAction::ReturnToLauncher, std::string())) {
            if (auto logger = spdlog::default_logger())
                logger->flush();

            // A successful restart does not return.
            const Result restart_rc = appletRestartProgram(nullptr, 0);
            LOG_WARN("Installer done; RestartProgram returned {}, trying RequestLaunchApplication", log_hex(restart_rc));

            const Result request_rc = appletRequestLaunchApplication(0, nullptr);
            if (R_SUCCEEDED(request_rc) && !return_path.empty() && envHasNextLoad()) {
                envSetNextLoad(return_path.c_str(), HBLOADER_EXIT_SENTINEL);
                application_relaunch_queued = true;
                LOG_INFO("Installer done; queued a clean application relaunch");
            } else {
                std::remove(SWITCH_FRONTEND_REQUEST_PATH);
                LOG_ERROR("Installer done; clean relaunch unavailable (rc={}), falling back to the in-process chainload",
                    log_hex(request_rc));
            }
        }

        if (!application_relaunch_queued && !return_path.empty()) {
            if (envHasNextLoad()) {
                LOG_INFO("Installer done; returning to launcher: {}", return_path);
                envSetNextLoad(return_path.c_str(), return_path.c_str());
            } else {
                LOG_WARN("--return given but envHasNextLoad() is false; cannot chainload {}", return_path);
            }
        }

        if (auto logger = spdlog::default_logger())
            logger->flush();

        socketExit();
        romfsExit();
        return 0;
    }

    // Title from -r/autoboot.txt, otherwise show the on-screen picker.
    std::string title_id = resolve_title_id(argc, argv);
    if (title_id.empty())
        title_id = select_title_from_menu();

    int exit_code = 0;
    SwitchRunResult run_result{};
    if (!title_id.empty()) {
        NWindow *nwindow = nwindowGetDefault();
        run_result = run_game(title_id, nwindow);
        exit_code = run_result.exit_code;
    }

    // From here the process is handed back to hbloader. An exception escaping
    // this path reaches std::terminate and takes the console down instead of
    // returning to the launcher, so every stage is guarded. Losing one teardown
    // stage is recoverable; losing the handoff is not.
    const auto shutdown_step = [](const char *stage) {
        // The breadcrumb survives even when the log line after it does not.
        logging::set_crash_breadcrumb(stage);
        LOG_WARN("[shutdown] {}", stage);
    };
    const auto guarded = [&](const char *stage, auto &&work) {
        shutdown_step(stage);
        try {
            work();
        } catch (const std::exception &e) {
            LOG_CRITICAL("[shutdown] '{}' threw: {}", stage, e.what());
        } catch (...) {
            LOG_CRITICAL("[shutdown] '{}' threw an unknown exception", stage);
        }
    };

    // Destroy the long-lived session too, not just its per-game runtime. This
    // drops cached Vulkan device data and any remaining owners of guest threads
    // before the process is handed back to hbloader.
    guarded("stopping the app session", [&] { session.app_session_controller.reset(); });
    guarded("releasing the emulator state", [&] { session.emuenv.reset(); });
    guarded("stopping vibration", [] { switch_stop_vibration(); });

    // Dynarmic's per-thread CodeBlocks share large libnx Jit regions. Returning
    // their slices is not enough: hbloader needs the regions and both aliases
    // unmapped before it can place the launcher NRO in this process.
    shutdown_step("releasing the JIT code pool");
    const SwitchJitPoolShutdownResult jit_shutdown = switch_shutdown_jit_code_pool();
    if (jit_shutdown.live_slices != 0) {
        LOG_ERROR("Could not release Dynarmic JIT pool: {} code slices are still live", jit_shutdown.live_slices);
    } else if (jit_shutdown.close_result != 0) {
        LOG_ERROR("Could not fully release Dynarmic JIT pool: jitClose returned {} after closing {} regions",
            log_hex(jit_shutdown.close_result), jit_shutdown.regions_closed);
    } else {
        LOG_INFO("Released {} Dynarmic JIT code-memory regions", jit_shutdown.regions_closed);
    }

    // The early reservation spans the process lifetime rather than a single
    // title. Release it before asking hbloader to start the launcher again.
    guarded("releasing the guest memory reservation", [] { switch_release_guest_region(); });

    // init_switch_session repartitions hbloader's heap to leave physical headroom
    // for NVK. hbloader keeps its original size in g_heapSize and uses it to load
    // the next NRO, so the kernel heap must match that value again.
    bool loader_heap_restored = false;
    guarded("restoring the hbloader heap", [&] { loader_heap_restored = switch_restore_loader_heap(); });
    if (!loader_heap_restored)
        LOG_ERROR("Returning with an unrestored hbloader heap; the next NRO may not load");
    shutdown_step("choosing the return path");

    // A naturally-ended guest and both launcher actions return to the frontend.
    // A HOME/quick-menu Exit request must instead leave the Application. In the
    // installed-forwarder case, restart the whole Application so hbloader never
    // has to remap the launcher in Vita3K's modified process.
    const bool wants_launcher = run_result.action == SwitchFrontendAction::ReturnToLauncher
        || (run_result.action == SwitchFrontendAction::None && !run_result.title_id.empty());

    if (wants_launcher) {
        bool clean_restart_ready = current_forwarder_targets(return_path);
        if (clean_restart_ready)
            clean_restart_ready = write_frontend_request(run_result.action, run_result.title_id);
        bool application_relaunch_queued = false;

        if (clean_restart_ready) {
            LOG_INFO("Restarting the current Switch application for a clean launcher process");
            if (auto logger = spdlog::default_logger())
                logger->flush();

            // This enters an infinite sleep on success while Horizon replaces the
            // current Application. A return means this command was unavailable;
            // try the older request-launch API before considering an in-process
            // hbloader transition.
            const Result restart_rc = appletRestartProgram(nullptr, 0);
            LOG_WARN("RestartProgram returned {}; trying RequestLaunchApplication", log_hex(restart_rc));

            const Result request_rc = appletRequestLaunchApplication(0, nullptr);
            if (R_SUCCEEDED(request_rc) && !return_path.empty() && envHasNextLoad()) {
                // Make the old hbloader exit its process instead of trying to map
                // another NRO while applet manager services the queued relaunch.
                envSetNextLoad(return_path.c_str(), HBLOADER_EXIT_SENTINEL);
                application_relaunch_queued = true;
                LOG_INFO("Queued clean current-application relaunch through RequestLaunchApplication");
            } else {
                std::remove(SWITCH_FRONTEND_REQUEST_PATH);
                LOG_ERROR("RequestLaunchApplication failed or hbloader exit is unavailable (rc={}); clean relaunch unavailable",
                    log_hex(request_rc));
            }
        }

        if (application_relaunch_queued) {
            // envSetNextLoad already contains hbloader's process-exit sentinel.
        } else if (return_path.empty()) {
            LOG_WARN("Launcher return requested, but no --return path was supplied");
        } else if (!envHasNextLoad()) {
            LOG_WARN("Cannot return to launcher because envHasNextLoad() is false");
        } else {
            LOG_INFO("Falling back to in-process launcher chainload: {}", return_path);
            envSetNextLoad(return_path.c_str(), return_path.c_str());
        }
    } else if (run_result.action == SwitchFrontendAction::ExitApplication) {
        // Armed only as a fallback for the ISelfController::Exit below.
        if (!return_path.empty() && envHasNextLoad())
            envSetNextLoad(return_path.c_str(), HBLOADER_EXIT_SENTINEL);
    }

    // Make the teardown diagnostics durable before hbloader replaces this NRO.
    if (auto logger = spdlog::default_logger())
        logger->flush();

    socketExit();
    romfsExit();

    if (run_result.action == SwitchFrontendAction::ExitApplication) {
        // Terminate the Application ourselves. Any hbloader regaining control
        // maps its next NRO with svcSetProcessMemoryPermission, which fails with
        // 0xD401 once Vita3K has exercised JIT and alias mappings here.
        Service *const self_controller = appletGetServiceSession_SelfController();
        const Result exit_rc = self_controller
            ? serviceDispatch(self_controller, 0)
            : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        if (R_SUCCEEDED(exit_rc)) {
            while (true)
                svcSleepThread(86400000000000ULL);
        }
        LOG_ERROR("ISelfController::Exit failed (rc=0x{:X}); falling back to hbloader", exit_rc);
    }

    return exit_code;
}
