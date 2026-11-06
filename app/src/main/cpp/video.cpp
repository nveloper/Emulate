#include "video.h"

#include <android/log.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>

#define LOG_TAG "EmuVideo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---- shaders (GLSL ES 1.00) -----------------------------------------------------------------

const char* kVertexShader = R"(
attribute vec2 aPos;
varying vec2 vUV;
void main() {
    vUV = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kFragmentHeader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTex;
uniform vec2 uTexSize;
uniform vec2 uOutSize;
uniform float uSwizzle;
uniform vec2 uViewport;  // bottom-left corner of the viewport, in window pixels
varying vec2 vUV;
// Texture coordinate of this screen pixel's centre, exact (unlike the interpolated vUV, which can
// round differently in the quad's two triangles and put a stray line along the diagonal).
vec2 pixelUV() {
    return vec2(gl_FragCoord.x - uViewport.x, uViewport.y + uOutSize.y - gl_FragCoord.y) / uOutSize;
}
vec3 fetch(vec2 uv) {
    vec4 c = texture2D(uTex, uv);
    return mix(c.rgb, c.bgr, uSwizzle);  // XRGB8888 is uploaded as bytes B,G,R,X
}
)";

// Indexed by VideoOutput::Filter.
const char* kFragmentBodies[4] = {
    // Sharp: nearest-neighbour look at any (non-integer) scale. Inside a texel the colour is flat;
    // only the last output pixel before a texel edge is blended (bilinear sampling), so every game
    // pixel ends up the same size without shimmering.
    R"(
void main() {
    vec2 texel = pixelUV() * uTexSize;
    vec2 scale = max(uOutSize / uTexSize, vec2(1.0));
    vec2 region = 0.5 - 0.5 / scale;
    vec2 d = fract(texel) - 0.5;
    vec2 f = (d - clamp(d, -region, region)) * scale + 0.5;
    gl_FragColor = vec4(fetch((floor(texel) + f) / uTexSize), 1.0);
}
)",
    // Nearest (texture filtering does the work).
    R"(
void main() { gl_FragColor = vec4(fetch(vUV), 1.0); }
)",
    // Smooth (bilinear).
    R"(
void main() { gl_FragColor = vec4(fetch(vUV), 1.0); }
)",
    // LCD: flat pixels with a faint dark grid between them, like a handheld's LCD. The grid fades
    // out when the picture is too small to fit it.
    R"(
void main() {
    vec2 texel = pixelUV() * uTexSize;
    vec3 c = fetch((floor(texel) + 0.5) / uTexSize);
    vec2 scale = uOutSize / uTexSize;
    // 1 on the first screen pixel of each game pixel: exactly one line per pixel boundary.
    // (Slightly under 1/scale: at scales like 4.21875 a boundary can fall exactly on a pixel centre
    // and rounding would otherwise mark two neighbouring pixels.)
    vec2 first = vec2(1.0) - step(vec2(0.999) / scale, fract(texel));
    float line = max(first.x, first.y);
    float strength = clamp(min(scale.x, scale.y) - 2.0, 0.0, 1.0) * 0.35;
    gl_FragColor = vec4(c * (1.0 - strength * line), 1.0);
}
)",
};

// Ambient light around the picture: the edge colours, spreading (blurring) and fading towards
// black with distance from the picture. Positions in pixels from the top left.
const char* kAmbientShader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uStrip;
uniform float uRow;       // which edge's row in the strip texture
uniform vec4 uGame;       // picture: x, y, width, height
uniform vec2 uScreen;     // surface size
uniform float uMode;      // 0: below the picture, 1: left of it, 2: right of it, 3: above it
uniform float uStrength;
void main() {
    vec2 p = vec2(gl_FragCoord.x, uScreen.y - gl_FragCoord.y);
    float u;
    float t;
    if (uMode < 0.5) {
        u = (p.x - uGame.x) / uGame.z;
        t = (p.y - uGame.y - uGame.w) / max(uScreen.y - uGame.y - uGame.w, 1.0);
    } else if (uMode < 1.5) {
        u = (p.y - uGame.y) / uGame.w;
        t = (uGame.x - p.x) / max(uGame.x, 1.0);
    } else if (uMode < 2.5) {
        u = (p.y - uGame.y) / uGame.w;
        t = (p.x - uGame.x - uGame.z) / max(uScreen.x - uGame.x - uGame.z, 1.0);
    } else {
        u = (p.x - uGame.x) / uGame.z;
        t = (uGame.y - p.y) / max(uGame.y, 1.0);
    }
    t = clamp(t, 0.0, 1.0);
    float spread = 0.03 + t * 0.3;
    vec3 c = texture2D(uStrip, vec2(clamp(u - spread, 0.0, 1.0), uRow)).rgb
           + texture2D(uStrip, vec2(clamp(u, 0.0, 1.0), uRow)).rgb * 2.0
           + texture2D(uStrip, vec2(clamp(u + spread, 0.0, 1.0), uRow)).rgb;
    c *= 0.25;
    // Tame bright edges (a white screen shouldn't light up the controls' background).
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c /= 1.0 + luma * 0.8;
    float fade = pow(1.0 - t, 1.7);
    gl_FragColor = vec4(c * uStrength * fade, 1.0);
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(GLuint vs, const char* fragmentBody, bool withHeader = true) {
    std::string source = withHeader ? std::string(kFragmentHeader) + fragmentBody : std::string(fragmentBody);
    GLuint fs = compile(GL_FRAGMENT_SHADER, source.c_str());
    if (!fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glLinkProgram(program);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// ---- CPU fallback pixel conversion to RGBX_8888 (bytes R,G,B,X => little-endian 0xXXBBGGRR) ----

inline uint32_t fromXrgb8888(uint32_t p) {
    return 0xFF000000u | (p & 0x0000FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
}

inline uint32_t fromRgb565(uint16_t p) {
    uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

inline uint16_t rgb565From0rgb1555(uint16_t p) {
    uint16_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
    return static_cast<uint16_t>((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

}  // namespace

// ---- threads --------------------------------------------------------------------------------

void VideoOutput::ensureThreads() {
    std::call_once(threadOnce_, [this] {
        wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        // Both live for the whole process, like the Frontend singleton that owns them.
        vsyncThread_ = std::thread(&VideoOutput::vsyncThreadMain, this);
        vsyncThread_.detach();
        drawThread_ = std::thread(&VideoOutput::drawThreadMain, this);
        drawThread_.detach();
    });
}

void VideoOutput::vsyncThreadMain() {
    ALooper* looper = ALooper_prepare(0);
    ALooper_addFd(looper, wakeFd_, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, onWake, this);
    choreographer_ = AChoreographer_getInstance();
    scheduleCallback();
    for (;;) ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
}

void VideoOutput::wakeVsyncThread() {
    ensureThreads();
    uint64_t one = 1;
    (void)!write(wakeFd_, &one, sizeof(one));
}

int VideoOutput::onWake(int fd, int, void* data) {
    uint64_t value;
    (void)!read(fd, &value, sizeof(value));
    static_cast<VideoOutput*>(data)->scheduleCallback();
    return 1;  // keep listening
}

void VideoOutput::onFrame(long frameTimeNanos, void* data) {
    static_cast<VideoOutput*>(data)->handleFrame(static_cast<int64_t>(frameTimeNanos));
}

void VideoOutput::scheduleCallback() {
    // Refresh callbacks run only while a game is running (paused: nothing changes on screen).
    if (!active_.load() || callbackPending_ || !choreographer_) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The 64-bit variant needs API 29; on 64-bit ABIs (the only ones built) `long` is 64-bit.
    AChoreographer_postFrameCallback(choreographer_, onFrame, this);
#pragma clang diagnostic pop
    callbackPending_ = true;
}

void VideoOutput::handleFrame(int64_t frameTimeNanos) {
    callbackPending_ = false;
    scheduleCallback();

    // Refresh rate: median of recent vsync intervals (robust to missed callbacks and gaps).
    if (lastFrameTime_ != 0) {
        const int64_t delta = frameTimeNanos - lastFrameTime_;
        if (delta > 2'000'000 && delta < 100'000'000) {
            deltas_[deltaIndex_] = delta;
            deltaIndex_ = (deltaIndex_ + 1) % 15;
            deltaCount_ = std::min(deltaCount_ + 1, 15);
            if (deltaCount_ >= 5) {
                int64_t sorted[15];
                std::copy(deltas_, deltas_ + deltaCount_, sorted);
                std::nth_element(sorted, sorted + deltaCount_ / 2, sorted + deltaCount_);
                refreshHz_ = 1e9 / static_cast<double>(sorted[deltaCount_ / 2]);
            }
        }
    }
    lastFrameTime_ = frameTimeNanos;
    {
        // Re-anchor the clock on the real vsync time. A late callback can stand for several
        // refreshes; count them all.
        std::lock_guard<std::mutex> lock(vsyncMutex_);
        const double hz = refreshHz_.load();
        if (anchorTime_ != 0 && hz > 0) {
            anchorCount_ += std::max<int64_t>(1, std::llround(static_cast<double>(frameTimeNanos - anchorTime_) * hz / 1e9));
        } else {
            anchorCount_++;
        }
        anchorTime_ = frameTimeNanos;
    }
    {
        std::lock_guard<std::mutex> lock(drawMutex_);
        drawTicks_++;
    }
    drawCv_.notify_one();
}

void VideoOutput::requestDraw() {
    ensureThreads();
    {
        std::lock_guard<std::mutex> lock(drawMutex_);
        drawRequested_ = true;
    }
    drawCv_.notify_one();
}

void VideoOutput::drawThreadMain() {
    uint64_t seenTicks = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(drawMutex_);
            drawCv_.wait(lock, [&] { return drawTicks_ != seenTicks || drawRequested_; });
            seenTicks = drawTicks_;
            // Something changed (game rectangle, ambient light, window...): draw the current frame
            // again even without a new one, e.g. after rotating while the game is paused.
            if (drawRequested_) redraw_ = true;
            drawRequested_ = false;
        }
        applyWindowChange();
        applyFrameRate();
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            if (fresh_) {
                std::swap(ready_, shown_);
                fresh_ = false;
                got = true;
            }
        }
        if (filter_.load() != appliedFilter_) redraw_ = true;
        if (window_ && (got || redraw_)) {
            draw();
            if (got) presented_++;
            redraw_ = false;
        }
    }
}

uint64_t VideoOutput::vsyncCount() {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();  // CLOCK_MONOTONIC
    std::lock_guard<std::mutex> lock(vsyncMutex_);
    uint64_t count = anchorCount_;
    const double hz = refreshHz_.load();
    if (anchorTime_ != 0 && hz > 0 && now > anchorTime_) {
        count += static_cast<uint64_t>(static_cast<double>(now - anchorTime_) * hz / 1e9);
    }
    lastCount_ = std::max(lastCount_, count);
    return lastCount_;
}

bool VideoOutput::waitForVsync(uint64_t target, std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    for (;;) {
        if (vsyncCount() >= target) return true;
        auto wake = deadline;
        {
            std::lock_guard<std::mutex> lock(vsyncMutex_);
            const double hz = refreshHz_.load();
            if (anchorTime_ != 0 && hz > 0) {
                // Predicted time of the target refresh (+50 us so the count has surely ticked).
                const double ns = static_cast<double>(anchorTime_) +
                                  static_cast<double>(target - anchorCount_) * 1e9 / hz + 50'000.0;
                wake = std::min(deadline, clock::time_point(std::chrono::nanoseconds(static_cast<int64_t>(ns))));
            } else {
                wake = std::min(deadline, clock::now() + std::chrono::milliseconds(2));
            }
        }
        if (clock::now() >= deadline) return false;
        std::this_thread::sleep_until(wake);
    }
}

// ---- requests from other threads -------------------------------------------------------------

void VideoOutput::setWindow(ANativeWindow* window) {
    ensureThreads();
    uint64_t request;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (windowPending_ && pendingWindow_) ANativeWindow_release(pendingWindow_);
        pendingWindow_ = window;
        windowPending_ = true;
        request = ++windowRequested_;
    }
    requestDraw();
    std::unique_lock<std::mutex> lock(windowMutex_);
    if (!windowCv_.wait_for(lock, std::chrono::seconds(2), [&] { return windowApplied_ >= request; })) {
        LOGE("Draw thread did not take the new window in time");
    }
}

void VideoOutput::setFilter(int filter) {
    filter_ = std::clamp(filter, 0, 3);
    requestDraw();
}

void VideoOutput::setContentFrameRate(float fps) {
    contentFps_ = fps;
    requestDraw();
}

void VideoOutput::setActive(bool active) {
    active_ = active;
    if (active) wakeVsyncThread();
}

void VideoOutput::clear() {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        for (auto& frame : frames_) frame.width = frame.height = 0;
        fresh_ = true;
    }
    requestDraw();
}

void VideoOutput::submit(const void* data, unsigned width, unsigned height, size_t pitch,
                         retro_pixel_format format, bool onlyIfShown) {
    if (onlyIfShown) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (fresh_) return;
    }
    Frame& frame = frames_[write_];  // only this thread changes write_
    const bool xrgb = format == RETRO_PIXEL_FORMAT_XRGB8888;
    const size_t bpp = xrgb ? 4 : 2;
    const size_t rowBytes = width * bpp;
    frame.pixels.resize(rowBytes * height);
    const auto* src = static_cast<const uint8_t*>(data);
    uint8_t* dst = frame.pixels.data();
    if (format == RETRO_PIXEL_FORMAT_0RGB1555) {
        for (unsigned y = 0; y < height; y++) {
            const auto* in = reinterpret_cast<const uint16_t*>(src + y * pitch);
            auto* out = reinterpret_cast<uint16_t*>(dst + y * rowBytes);
            for (unsigned x = 0; x < width; x++) out[x] = rgb565From0rgb1555(in[x]);
        }
    } else if (pitch == rowBytes) {
        memcpy(dst, src, rowBytes * height);
    } else {
        for (unsigned y = 0; y < height; y++) memcpy(dst + y * rowBytes, src + y * pitch, rowBytes);
    }
    frame.width = width;
    frame.height = height;
    frame.xrgb = xrgb;
    frame.serial = ++frameSerial_;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        std::swap(write_, ready_);
        fresh_ = true;
    }
    // While running, the next refresh picks it up; otherwise (e.g. a frame produced while the
    // game is paused) draw it now.
    if (!active_.load()) requestDraw();
}

bool VideoOutput::capture(std::vector<uint32_t>& argb, unsigned& width, unsigned& height) {
    // Frames other than frames_[write_] only change under frameMutex_, so this copy is safe.
    std::lock_guard<std::mutex> lock(frameMutex_);
    const Frame& frame = frames_[fresh_ ? ready_ : shown_];
    if (frame.width == 0 || frame.height == 0) return false;
    width = frame.width;
    height = frame.height;
    argb.resize(static_cast<size_t>(width) * height);
    const size_t count = argb.size();
    if (frame.xrgb) {
        const auto* src = reinterpret_cast<const uint32_t*>(frame.pixels.data());
        for (size_t i = 0; i < count; i++) argb[i] = 0xFF000000u | (src[i] & 0x00FFFFFFu);
    } else {
        const auto* src = reinterpret_cast<const uint16_t*>(frame.pixels.data());
        for (size_t i = 0; i < count; i++) {
            uint32_t p = src[i];
            uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

// ---- window / EGL ---------------------------------------------------------------------------

void VideoOutput::applyWindowChange() {
    ANativeWindow* window;
    uint64_t request;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (!windowPending_) return;
        window = pendingWindow_;
        pendingWindow_ = nullptr;
        windowPending_ = false;
        request = windowRequested_;
    }
    if (window && window == window_) {
        ANativeWindow_release(window);  // same surface (e.g. resized): keep the EGL surface
    } else {
        destroySurface();
        if (window_) ANativeWindow_release(window_);
        window_ = window;
        cpuBufferWidth_ = cpuBufferHeight_ = 0;
        appliedFrameRate_ = 0.0f;
        if (window_) {
            gpu_ = !eglFailed_ && initEgl() && createSurface();
            LOGI("Presenting with %s", gpu_.load() ? "OpenGL ES" : "CPU blit");
        }
    }
    redraw_ = window_ != nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        windowApplied_ = request;
    }
    windowCv_.notify_all();
}

void VideoOutput::applyFrameRate() {
    float fps = contentFps_.load();
    if (!window_ || fps <= 0.0f || fps == appliedFrameRate_) return;
    appliedFrameRate_ = fps;
    // API 31+ / 30+. FIXED_SOURCE (1): the content has a fixed rate, so the system may switch the
    // display to a matching mode (e.g. 120 Hz -> 60 Hz); ALWAYS (1): even if the switch isn't seamless.
    using WithStrategy = int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
    using Plain = int32_t (*)(ANativeWindow*, float, int8_t);
    static void* android = dlopen("libandroid.so", RTLD_NOW);
    static auto withStrategy = android ? reinterpret_cast<WithStrategy>(dlsym(android, "ANativeWindow_setFrameRateWithChangeStrategy")) : nullptr;
    static auto plain = android ? reinterpret_cast<Plain>(dlsym(android, "ANativeWindow_setFrameRate")) : nullptr;
    int32_t result = -1;
    if (withStrategy) result = withStrategy(window_, fps, 1, 1);
    if (result != 0 && plain) result = plain(window_, fps, 1);
    LOGI("Requested display frame rate %.3f (result %d, api %d/%d)", fps, result, withStrategy != nullptr, plain != nullptr);
}

bool VideoOutput::initEgl() {
    if (context_ != EGL_NO_CONTEXT) return true;
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        eglFailed_ = true;
        return false;
    }
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(display_, attribs, &config_, 1, &count) || count < 1) {
        LOGE("eglChooseConfig failed");
        eglFailed_ = true;
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        eglFailed_ = true;
        return false;
    }
    return true;
}

bool VideoOutput::createSurface() {
    EGLint format = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(window_, 0, 0, format);
    surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        destroySurface();
        return false;
    }
    eglSwapInterval(display_, 1);
    if (!glReady_ && !initGl()) {
        destroySurface();
        eglFailed_ = true;
        return false;
    }
    return true;
}

void VideoOutput::destroySurface() {
    if (surface_ == EGL_NO_SURFACE) return;
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display_, surface_);
    surface_ = EGL_NO_SURFACE;
}

bool VideoOutput::initGl() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    if (!vs) return false;
    for (int i = 0; i < 4; i++) {
        programs_[i] = linkProgram(vs, kFragmentBodies[i]);
        if (!programs_[i]) {
            glDeleteShader(vs);
            return false;
        }
        uniforms_[i][0] = glGetUniformLocation(programs_[i], "uTex");
        uniforms_[i][1] = glGetUniformLocation(programs_[i], "uTexSize");
        uniforms_[i][2] = glGetUniformLocation(programs_[i], "uOutSize");
        uniforms_[i][3] = glGetUniformLocation(programs_[i], "uSwizzle");
        uniforms_[i][4] = glGetUniformLocation(programs_[i], "uViewport");
    }
    ambientProgram_ = linkProgram(vs, kAmbientShader, false);
    glDeleteShader(vs);
    if (!ambientProgram_) return false;
    const char* ambientNames[6] = {"uStrip", "uRow", "uGame", "uScreen", "uMode", "uStrength"};
    for (int i = 0; i < 6; i++) ambientUniforms_[i] = glGetUniformLocation(ambientProgram_, ambientNames[i]);

    glGenTextures(1, &stripTexture_);
    glBindTexture(GL_TEXTURE_2D, stripTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const GLfloat quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    glGenBuffers(1, &vertexBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glReady_ = true;
    return true;
}

// ---- drawing --------------------------------------------------------------------------------

void VideoOutput::draw() {
    appliedFilter_ = filter_.load();
    const Frame& frame = frames_[shown_];  // owned by this thread between swaps
    if (gpu_.load() && surface_ != EGL_NO_SURFACE) {
        drawGl(frame);
    } else {
        drawCpu(frame);
    }
}

void VideoOutput::drawGl(const Frame& frame) {
    EGLint width = 0, height = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &width);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    int gx = gameX_, gy = gameY_, gw = gameW_, gh = gameH_;
    if (gw <= 0 || gh <= 0) {
        gx = gy = 0;
        gw = width;
        gh = height;
    }

    if (frame.width > 0 && frame.height > 0) {
        if (ambient_.load()) {
            if (frame.serial != stripSerial_) updateStrips(frame);
            drawAmbient(width, height, gx, gy, gw, gh);
        }
        const int vx = gx, vy = height - gy - gh;  // GL's origin is the bottom left
        glViewport(vx, vy, gw, gh);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        if (frame.serial != uploadedSerial_ || frame.width != texWidth_ || frame.height != texHeight_ ||
            frame.xrgb != texXrgb_) {
            const GLenum format = frame.xrgb ? GL_RGBA : GL_RGB;
            const GLenum type = frame.xrgb ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5;
            glPixelStorei(GL_UNPACK_ALIGNMENT, frame.xrgb ? 4 : 2);
            if (frame.width != texWidth_ || frame.height != texHeight_ || frame.xrgb != texXrgb_) {
                glTexImage2D(GL_TEXTURE_2D, 0, format, frame.width, frame.height, 0, format, type, frame.pixels.data());
                texWidth_ = frame.width;
                texHeight_ = frame.height;
                texXrgb_ = frame.xrgb;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, format, type, frame.pixels.data());
            }
            uploadedSerial_ = frame.serial;
        }
        const int filter = appliedFilter_;
        const GLint sampling = (filter == kSharp || filter == kSmooth) ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sampling);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampling);

        glUseProgram(programs_[filter]);
        glUniform1i(uniforms_[filter][0], 0);
        glUniform2f(uniforms_[filter][1], static_cast<float>(frame.width), static_cast<float>(frame.height));
        glUniform2f(uniforms_[filter][2], static_cast<float>(gw), static_cast<float>(gh));
        glUniform1f(uniforms_[filter][3], frame.xrgb ? 1.f : 0.f);
        glUniform2f(uniforms_[filter][4], static_cast<float>(vx), static_cast<float>(vy));
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (!eglSwapBuffers(display_, surface_)) LOGE("eglSwapBuffers failed: 0x%x", eglGetError());
}

void VideoOutput::drawCpu(const Frame& frame) {
    // Fallback without OpenGL: nearest-neighbour into the game rectangle of a half-resolution
    // full-screen buffer (the compositor scales it up), black elsewhere. No ambient light.
    const int vw = viewWidth_, vh = viewHeight_;
    if (frame.width == 0 || frame.height == 0 || vw <= 0 || vh <= 0) return;
    const int bw = std::max(1, vw / 2), bh = std::max(1, vh / 2);
    if (bw != cpuBufferWidth_ || bh != cpuBufferHeight_) {
        ANativeWindow_setBuffersGeometry(window_, bw, bh, WINDOW_FORMAT_RGBX_8888);
        cpuBufferWidth_ = bw;
        cpuBufferHeight_ = bh;
    }
    int gx = gameX_ / 2, gy = gameY_ / 2, gw = gameW_ / 2, gh = gameH_ / 2;
    if (gw <= 0 || gh <= 0) {
        gx = gy = 0;
        gw = bw;
        gh = bh;
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) return;
    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int outW = std::min(bw, buffer.width), outH = std::min(bh, buffer.height);
    const size_t rowBytes = frame.width * (frame.xrgb ? 4 : 2);
    for (int y = 0; y < outH; y++) {
        uint32_t* line = dst + static_cast<size_t>(y) * buffer.stride;
        if (y < gy || y >= gy + gh) {
            std::fill(line, line + outW, 0xFF000000u);
            continue;
        }
        const uint8_t* row = frame.pixels.data() + static_cast<size_t>((y - gy) * static_cast<int>(frame.height) / gh) * rowBytes;
        for (int x = 0; x < outW; x++) {
            if (x < gx || x >= gx + gw) {
                line[x] = 0xFF000000u;
                continue;
            }
            const int sx = (x - gx) * static_cast<int>(frame.width) / gw;
            line[x] = frame.xrgb ? fromXrgb8888(reinterpret_cast<const uint32_t*>(row)[sx])
                                 : fromRgb565(reinterpret_cast<const uint16_t*>(row)[sx]);
        }
    }
    ANativeWindow_unlockAndPost(window_);
}

// ---- ambient light ----------------------------------------------------------------------------

namespace {
inline void rgbAt(const uint8_t* pixels, size_t rowBytes, bool xrgb, int x, int y, float out[3]) {
    if (xrgb) {
        uint32_t p = reinterpret_cast<const uint32_t*>(pixels + y * rowBytes)[x];
        out[0] += ((p >> 16) & 0xFF) / 255.f;
        out[1] += ((p >> 8) & 0xFF) / 255.f;
        out[2] += (p & 0xFF) / 255.f;
    } else {
        uint16_t p = reinterpret_cast<const uint16_t*>(pixels + y * rowBytes)[x];
        out[0] += ((p >> 11) & 0x1F) / 31.f;
        out[1] += ((p >> 5) & 0x3F) / 63.f;
        out[2] += (p & 0x1F) / 31.f;
    }
}
}  // namespace

void VideoOutput::updateStrips(const Frame& frame) {
    stripSerial_ = frame.serial;
    const int w = static_cast<int>(frame.width), h = static_cast<int>(frame.height);
    const size_t rowBytes = frame.width * (frame.xrgb ? 4 : 2);
    const uint8_t* px = frame.pixels.data();
    const int depth = std::max(1, std::min(w, h) / 16);  // how far into the picture to look
    float fresh[kEdges][kStrip][3] = {};
    for (int i = 0; i < kStrip; i++) {
        // Bottom edge: column band i, last `depth` rows.
        const int x0 = i * w / kStrip, x1 = std::max(x0 + 1, (i + 1) * w / kStrip);
        for (int y = h - depth; y < h; y++)
            for (int x = x0; x < x1; x += 2) rgbAt(px, rowBytes, frame.xrgb, x, y, fresh[0][i]);
        for (int y = 0; y < depth; y++)
            for (int x = x0; x < x1; x += 2) rgbAt(px, rowBytes, frame.xrgb, x, y, fresh[3][i]);
        // Left and right edges: row band i, first / last `depth` columns.
        const int y0 = i * h / kStrip, y1 = std::max(y0 + 1, (i + 1) * h / kStrip);
        for (int y = y0; y < y1; y += 2) {
            for (int x = 0; x < depth; x++) rgbAt(px, rowBytes, frame.xrgb, x, y, fresh[1][i]);
            for (int x = w - depth; x < w; x++) rgbAt(px, rowBytes, frame.xrgb, x, y, fresh[2][i]);
        }
        const float bottomCount = static_cast<float>(depth * ((x1 - x0 + 1) / 2));
        const float sideCount = static_cast<float>(depth * ((y1 - y0 + 1) / 2));
        for (int c = 0; c < 3; c++) {
            fresh[0][i][c] /= bottomCount;
            fresh[3][i][c] /= bottomCount;
            fresh[1][i][c] /= sideCount;
            fresh[2][i][c] /= sideCount;
        }
    }
    // Ease towards the new colours so scene changes glow over rather than flicker.
    const float k = stripsValid_ ? 0.12f : 1.f;
    for (int e = 0; e < kEdges; e++)
        for (int i = 0; i < kStrip; i++)
            for (int c = 0; c < 3; c++) strips_[e][i][c] += (fresh[e][i][c] - strips_[e][i][c]) * k;
    stripsValid_ = true;

    uint8_t texels[kEdges][kStrip][4];
    for (int e = 0; e < kEdges; e++)
        for (int i = 0; i < kStrip; i++) {
            for (int c = 0; c < 3; c++) texels[e][i][c] = static_cast<uint8_t>(std::clamp(strips_[e][i][c], 0.f, 1.f) * 255.f + 0.5f);
            texels[e][i][3] = 255;
        }
    glBindTexture(GL_TEXTURE_2D, stripTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kStrip, kEdges, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
}

void VideoOutput::drawAmbient(int width, int height, int gx, int gy, int gw, int gh) {
    if (!stripsValid_) return;
    glUseProgram(ambientProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, stripTexture_);
    glUniform1i(ambientUniforms_[0], 0);
    glUniform4f(ambientUniforms_[2], static_cast<float>(gx), static_cast<float>(gy), static_cast<float>(gw), static_cast<float>(gh));
    glUniform2f(ambientUniforms_[3], static_cast<float>(width), static_cast<float>(height));
    glUniform1f(ambientUniforms_[5], 0.55f);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnable(GL_SCISSOR_TEST);
    // Regions in top-left pixel coordinates: x, y, w, h, mode (edge strip row = mode).
    const int regions[4][5] = {
        {0, gy + gh, width, height - gy - gh, 0},  // below the picture
        {0, gy, gx, gh, 1},                         // left of it
        {gx + gw, gy, width - gx - gw, gh, 2},      // right of it
        {0, 0, width, gy, 3},                       // above it
    };
    for (const auto& r : regions) {
        if (r[2] < 2 || r[3] < 2) continue;
        glScissor(r[0], height - r[1] - r[3], r[2], r[3]);
        glUniform1f(ambientUniforms_[1], (r[4] + 0.5f) / kEdges);
        glUniform1f(ambientUniforms_[4], static_cast<float>(r[4]));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_SCISSOR_TEST);
}

void VideoOutput::setGameRect(int x, int y, int width, int height) {
    gameX_ = x;
    gameY_ = y;
    gameW_ = width;
    gameH_ = height;
    requestDraw();
}

void VideoOutput::setAmbient(bool enabled) {
    ambient_ = enabled;
    requestDraw();
}
