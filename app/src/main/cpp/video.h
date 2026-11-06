#pragma once

#include "libretro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/choreographer.h>
#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Shows emulated frames on a dedicated draw thread.
//
// The emulation thread only copies each finished frame into a triple buffer (submit()). A vsync
// thread receives every display refresh (AChoreographer) and wakes the draw thread, which uploads
// the newest frame to a texture and draws it with OpenGL ES 2, so scaling and filtering cost the
// GPU rather than the CPU and the emulation thread never blocks on the display. If EGL is
// unavailable it falls back to a CPU nearest-neighbour blit into the window.
//
// The refresh callbacks double as a vsync clock that the emulation loop can pace itself on
// (vsyncCount() / waitForVsync()), which keeps frame delivery even when the game's frame rate
// (e.g. 59.73 Hz) is within a hair of the display's. Drawing happens on its own thread so a slow
// draw or a blocking buffer swap never delays that clock.
class VideoOutput {
public:
    enum Filter { kSharp = 0, kNearest = 1, kSmooth = 2, kLcd = 3 };

    // Takes ownership of one reference to [window] (may be null). Blocks until the render thread
    // has stopped using the previous window, as SurfaceHolder.Callback.surfaceDestroyed requires.
    void setWindow(ANativeWindow* window);
    void setViewSize(int width, int height) { viewWidth_ = width; viewHeight_ = height; }
    // Where the game goes on the (full-screen) surface, in pixels from the top left. An empty
    // rectangle means the whole surface.
    void setGameRect(int x, int y, int width, int height);
    // Ambient light: the colours at the picture's edges glow softly into the rest of the screen
    // (behind the touch controls), following the game live.
    void setAmbient(bool enabled);
    void setFilter(int filter);
    // The game's frame rate; the display is asked to switch to a matching refresh rate (API 30+).
    void setContentFrameRate(float fps);
    // While active (game running), refresh callbacks keep coming even without new frames.
    void setActive(bool active);
    // Forget the last game's picture.
    void clear();

    // Emulation thread. Copies the frame; with [onlyIfShown] the copy is skipped while the previous
    // frame hasn't been picked up by the display yet (fast-forward produces more than it can show).
    void submit(const void* data, unsigned width, unsigned height, size_t pitch, retro_pixel_format format,
                bool onlyIfShown);

    // Display refresh clock. Refreshes are counted from the last reported vsync timestamp and the
    // measured period, so a late or dropped callback doesn't slow the clock down.
    double refreshRate() const { return refreshHz_.load(); }  // measured Hz, 0 until known
    uint64_t vsyncCount();
    // Sleeps until vsyncCount() >= target (at most [timeout]). Returns whether it got there.
    bool waitForVsync(uint64_t target, std::chrono::milliseconds timeout);

    // Copies the newest frame as 0xAARRGGBB pixels (for save-state thumbnails). False if none.
    bool capture(std::vector<uint32_t>& argb, unsigned& width, unsigned& height);

    uint64_t presentedFrames() const { return presented_.load(); }
    bool gpu() const { return gpu_.load(); }

private:
    struct Frame {
        std::vector<uint8_t> pixels;  // tightly packed rows
        unsigned width = 0, height = 0;
        bool xrgb = false;            // false: RGB565, true: XRGB8888
        uint64_t serial = 0;
    };

    void ensureThreads();
    void vsyncThreadMain();
    void drawThreadMain();
    void wakeVsyncThread();
    void requestDraw();
    static int onWake(int fd, int events, void* data);
    static void onFrame(long frameTimeNanos, void* data);
    void handleFrame(int64_t frameTimeNanos);
    void scheduleCallback();
    void applyWindowChange();
    void applyFrameRate();

    bool initEgl();
    bool createSurface();
    void destroySurface();
    bool initGl();
    void draw();
    void drawGl(const Frame& frame);
    void drawCpu(const Frame& frame);

    std::once_flag threadOnce_;
    std::thread vsyncThread_, drawThread_;

    // Vsync thread.
    int wakeFd_ = -1;
    AChoreographer* choreographer_ = nullptr;
    bool callbackPending_ = false;
    int64_t lastFrameTime_ = 0;
    int64_t deltas_[15] = {};
    int deltaCount_ = 0, deltaIndex_ = 0;

    // Draw thread wake-ups: a refresh happened, or something needs drawing right away.
    std::mutex drawMutex_;
    std::condition_variable drawCv_;
    uint64_t drawTicks_ = 0;
    bool drawRequested_ = false;

    // Window hand-off from the UI thread.
    std::mutex windowMutex_;
    std::condition_variable windowCv_;
    ANativeWindow* pendingWindow_ = nullptr;
    bool windowPending_ = false;
    uint64_t windowRequested_ = 0, windowApplied_ = 0;

    // Draw thread state.
    ANativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    bool eglFailed_ = false;
    bool glReady_ = false;
    GLuint programs_[4] = {};
    GLint uniforms_[4][5] = {};  // per program: uTex, uTexSize, uOutSize, uSwizzle, uViewport
    GLuint ambientProgram_ = 0;
    GLint ambientUniforms_[6] = {};  // uStrip, uRow, uGame, uScreen, uMode, uStrength
    GLuint stripTexture_ = 0;
    // Edge colours (RGB, 0..1): bottom, left, right and top edge; smoothed over frames.
    static constexpr int kStrip = 32;
    static constexpr int kEdges = 4;
    float strips_[kEdges][kStrip][3] = {};
    bool stripsValid_ = false;
    uint64_t stripSerial_ = 0;
    void updateStrips(const Frame& frame);
    void drawAmbient(int width, int height, int gx, int gy, int gw, int gh);
    GLuint texture_ = 0;
    GLuint vertexBuffer_ = 0;
    unsigned texWidth_ = 0, texHeight_ = 0;
    bool texXrgb_ = false;
    uint64_t uploadedSerial_ = 0;
    int cpuBufferWidth_ = 0, cpuBufferHeight_ = 0;
    std::vector<uint32_t> lineBuffer_;
    bool redraw_ = false;
    int appliedFilter_ = -1;
    float appliedFrameRate_ = 0.0f;

    // Triple buffer: the emulation thread owns frames_[write_], the draw thread frames_[shown_].
    std::mutex frameMutex_;
    Frame frames_[3];
    int write_ = 0, ready_ = 1, shown_ = 2;
    bool fresh_ = false;
    uint64_t frameSerial_ = 0;  // emulation thread

    std::atomic<bool> active_{false};
    std::atomic<bool> clearRequested_{false};
    std::atomic<int> filter_{kSharp};
    std::atomic<float> contentFps_{0.0f};
    std::atomic<int> viewWidth_{0}, viewHeight_{0};
    std::atomic<int> gameX_{0}, gameY_{0}, gameW_{0}, gameH_{0};
    std::atomic<bool> ambient_{true};
    std::atomic<bool> gpu_{false};

    std::mutex vsyncMutex_;
    int64_t anchorTime_ = 0;       // CLOCK_MONOTONIC ns of the last reported vsync
    uint64_t anchorCount_ = 0;     // refreshes counted up to anchorTime_
    uint64_t lastCount_ = 0;       // keeps vsyncCount() monotonic
    std::atomic<double> refreshHz_{0.0};
    std::atomic<uint64_t> presented_{0};
};
