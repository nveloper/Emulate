#pragma once

#include "audio.h"
#include "libretro.h"
#include "netplay.h"
#include "video.h"

#include <android/native_window.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Function table of a dlopen()ed libretro core.
struct CoreApi {
    void* handle = nullptr;
    void (*init)();
    void (*deinit)();
    unsigned (*api_version)();
    void (*get_system_info)(retro_system_info*);
    void (*get_system_av_info)(retro_system_av_info*);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)();
    void (*run)();
    size_t (*serialize_size)();
    bool (*serialize)(void*, size_t);
    bool (*unserialize)(const void*, size_t);
    bool (*load_game)(const retro_game_info*);
    bool (*load_game_special)(unsigned, const retro_game_info*, size_t);
    void (*unload_game)();
    void* (*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    void (*cheat_reset)();                      // optional
    void (*cheat_set)(unsigned, bool, const char*);  // optional
};

// What to boot. Slot-2 fields are only used by the NDS core (GBA cartridge in the DS's GBA slot).
struct LaunchConfig {
    std::string corePath;       // e.g. "libmgba_libretro.so"
    std::string romPath;
    std::string savePath;       // battery save of the main ROM
    std::string rtcPath;        // GB real-time clock, if any
    std::string systemDir;
    std::string saveDir;
    std::string slot2RomPath;   // optional GBA ROM for NDS slot 2
    std::string slot2SavePath;  // its save (shared with the GBA core's save file)
    std::map<std::string, std::string> options;
    unsigned language = RETRO_LANGUAGE_ENGLISH;
};

class Frontend {
public:
    static Frontend& instance();

    // Loads the core and the game on a new emulation thread. Returns "" on success or an error.
    std::string start(const LaunchConfig& config);
    void stop();
    bool running() const { return running_; }

    void setPaused(bool paused);
    // Emulation speed: 1 = normal, >1 fast-forward, <1 slow motion. Forced to 1 during netplay.
    void setSpeed(float speed) { speed_ = std::clamp(speed, 0.25f, 16.0f); }
    // Whether sound keeps playing (time-stretched, same pitch) while fast-forwarding.
    void setSpeedAudio(bool enabled) { speedAudio_ = enabled; }
    void setAudioEnabled(bool enabled) { audioEnabled_ = enabled; }
    // Pace frames on the display's refresh when it matches the game's frame rate.
    void setVsyncPacing(bool enabled) { vsyncPacing_ = enabled; }
    void setVideoFilter(int filter) { video_.setFilter(filter); }
    void setWindow(ANativeWindow* window) { video_.setWindow(window); }
    void setViewSize(int width, int height) { video_.setViewSize(width, height); }
    void setGameRect(int x, int y, int width, int height) { video_.setGameRect(x, y, width, height); }
    void setAmbient(bool enabled) { video_.setAmbient(enabled); }
    bool captureFrame(std::vector<uint32_t>& argb, unsigned& width, unsigned& height) {
        return video_.capture(argb, width, height);
    }
    // [emulated fps, displayed fps, display Hz, avg core ms per frame, paced on vsync (0/1), speed]
    void stats(float out[6]);
    void setButtons(uint32_t mask) { buttons_ = mask; }
    void setPointer(int x, int y, bool pressed);
    void setOption(const std::string& key, const std::string& value);

    // Local wireless (NDS). Return "" on success or a user-facing error.
    std::string netplayHost(const std::string& title, const std::string& tag);
    std::string netplayJoin(const std::string& address, const std::string& tag);
    void netplayLeave();

    bool saveState(const std::string& path);
    bool loadState(const std::string& path);
    void reset();
    void flushSaves();
    // Replaces the active cheat codes (each one code, lines separated by whitespace).
    void setCheats(std::vector<std::string> codes);

    // libretro callbacks (static trampolines forward here)
    bool environment(unsigned cmd, void* data);
    void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
    size_t audioBatch(const int16_t* data, size_t frames);
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);

    // Java side (NativeBridge) for callbacks.
    JavaVM* vm = nullptr;
    jclass bridgeClass = nullptr;
    jmethodID onVideoSizeMethod = nullptr;
    jmethodID onMessageMethod = nullptr;
    jmethodID onNetplayStatusMethod = nullptr;
    jmethodID onRumbleMethod = nullptr;

    void setRumble(unsigned port, retro_rumble_effect effect, uint16_t strength);

private:
    Frontend() = default;
    void stopLocked();
    void threadMain(LaunchConfig config, std::promise<std::string> loaded);
    std::string loadCore(const std::string& path);
    std::string loadGame(const LaunchConfig& config);
    void unloadCore();
    void runCommands();
    // Runs fn on the emulation thread and waits for it.
    bool runOnEmuThread(std::function<bool()> fn);
    void loadSaveFile(unsigned memoryId, const std::string& path);
    void writeSaveFile(unsigned memoryId, const std::string& path, std::vector<uint8_t>& lastWritten);
    void postMessage(const char* message);
    void registerOptionDefault(const char* key, const char* value);

    CoreApi core_{};
    LaunchConfig config_;
    std::mutex lifecycleMutex_;  // serializes start()/stop()
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool> speedAudio_{false};
    std::atomic<bool> audioEnabled_{true};
    std::atomic<bool> vsyncPacing_{true};
    bool audioPlaying_ = true;  // emulation thread
    uint16_t rumble_[2] = {0, 0};  // strong, weak
    int lastRumble_ = 0;
    JNIEnv* env_ = nullptr;

    std::mutex cmdMutex_;
    std::condition_variable cmdCv_;
    std::deque<std::function<void()>> commands_;
    bool acceptingCommands_ = false;  // guarded by cmdMutex_

    // Core options: defaults declared by the core, overridden by the app.
    std::mutex optionsMutex_;
    std::map<std::string, std::string> optionDefaults_;
    std::map<std::string, std::string> optionOverrides_;
    std::map<std::string, std::string> optionValues_;  // storage for GET_VARIABLE results
    std::atomic<bool> optionsDirty_{false};

    // Content kept alive for cores that request persistent data. ROMs are memory-mapped rather
    // than read, so a 256 MB NDS ROM isn't held twice (cores copy it into their own buffer).
    struct MappedFile {
        void* data = nullptr;
        size_t size = 0;
        MappedFile() = default;
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;
        MappedFile(MappedFile&& other) noexcept : data(other.data), size(other.size) { other.data = nullptr; }
        ~MappedFile();
        bool map(const std::string& path);
    };
    std::vector<MappedFile> contentData_;
    std::vector<std::string> contentPaths_;
    struct Subsystem { std::string ident; unsigned id; };
    std::vector<Subsystem> subsystems_;

    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_system_av_info avInfo_{};
    retro_frame_time_callback frameTimeCallback_{};

    std::string lastMessage_;
    std::chrono::steady_clock::time_point lastMessageTime_{};
    std::vector<uint8_t> lastSram_;
    std::vector<uint8_t> lastRtc_;

    // Video output
    VideoOutput video_;
    unsigned frameWidth_ = 0, frameHeight_ = 0;
    // 0: pace on the clock; n: run one frame every n display refreshes.
    int vsyncDivisor(double coreFps, float speed) const;
    std::chrono::steady_clock::time_point vsyncSuspendedUntil_{};

    // Android performance hint session (API 33+): tells the CPU governor how long each frame may
    // take, so it ramps clocks for the actual load instead of guessing (e.g. after fast-forward).
    void* perfSession_ = nullptr;
    int64_t perfTarget_ = 0;
    void perfOpen(int64_t targetNs);
    void perfReport(int64_t actualNs, int64_t targetNs);
    void perfClose();

    // Statistics for the on-screen performance display.
    std::atomic<uint64_t> emuFrames_{0};
    std::atomic<uint64_t> workNs_{0};
    std::atomic<bool> pacingVsync_{false};
    std::mutex statsMutex_;
    std::chrono::steady_clock::time_point statsTime_{};
    uint64_t statsFrames_ = 0, statsPresented_ = 0, statsWork_ = 0;

    // Input
    std::atomic<uint32_t> buttons_{0};
    std::atomic<int> pointerX_{0}, pointerY_{0};
    std::atomic<bool> pointerPressed_{false};

    AudioOutput audio_;

    Netplay netplay_;
    retro_netpacket_callback netpacketCallbacks_{};
    bool hasNetpacket_ = false;
    std::atomic<bool> runSoon_{false};  // a link partner is waiting on us: don't sleep
    float effectiveSpeed() const { return netplay_.active() ? 1.0f : speed_.load(); }
};
