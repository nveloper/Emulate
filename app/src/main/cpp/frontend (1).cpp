#include "frontend.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>

#define LOG_TAG "EmuFrontend"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---- libretro trampolines -------------------------------------------------------------------

bool envTrampoline(unsigned cmd, void* data) { return Frontend::instance().environment(cmd, data); }

void videoTrampoline(const void* data, unsigned w, unsigned h, size_t pitch) {
    Frontend::instance().videoRefresh(data, w, h, pitch);
}

void audioSampleTrampoline(int16_t left, int16_t right) {
    int16_t frame[2] = {left, right};
    Frontend::instance().audioBatch(frame, 1);
}

size_t audioBatchTrampoline(const int16_t* data, size_t frames) {
    return Frontend::instance().audioBatch(data, frames);
}

void inputPollTrampoline() {}

int16_t inputStateTrampoline(unsigned port, unsigned device, unsigned index, unsigned id) {
    return Frontend::instance().inputState(port, device, index, id);
}

bool RETRO_CALLCONV rumbleTrampoline(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    Frontend::instance().setRumble(port, effect, strength);
    return true;
}

void coreLog(enum retro_log_level level, const char* fmt, ...) {
    static const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    int prio = level <= RETRO_LOG_ERROR ? priorities[level] : ANDROID_LOG_INFO;
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio, "EmuCore", fmt, args);
    va_end(args);
}

// ---- file helpers ---------------------------------------------------------------------------

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t read = size > 0 ? fread(out.data(), 1, out.size(), f) : 0;
    fclose(f);
    return read == out.size();
}

// Writes via a temp file + rename so a crash mid-write never corrupts an existing save.
bool writeFileAtomic(const std::string& path, const void* data, size_t size) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fflush(f) == 0) && ok;
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

std::mutex gCoreHandlesMutex;
std::map<std::string, void*> gCoreHandles;  // cores are never dlclose()d; see loadCore()

}  // namespace

Frontend::MappedFile::~MappedFile() {
    if (data) munmap(data, size);
}

bool Frontend::MappedFile::map(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }
    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    data = p;
    size = static_cast<size_t>(st.st_size);
    return true;
}

Frontend& Frontend::instance() {
    static Frontend frontend;
    return frontend;
}

// ---- lifecycle ------------------------------------------------------------------------------

std::string Frontend::start(const LaunchConfig& config) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
    std::promise<std::string> loaded;
    std::future<std::string> result = loaded.get_future();
    running_ = true;
    paused_ = false;
    thread_ = std::thread(&Frontend::threadMain, this, config, std::move(loaded));
    std::string error = result.get();
    if (!error.empty() && thread_.joinable()) thread_.join();
    return error;
}

void Frontend::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
}

void Frontend::stopLocked() {
    if (!thread_.joinable()) return;
    running_ = false;
    cmdCv_.notify_all();
    thread_.join();
}

void Frontend::setPaused(bool paused) {
    paused_ = paused;
    cmdCv_.notify_all();
}

void Frontend::threadMain(LaunchConfig config, std::promise<std::string> loaded) {
    if (vm) vm->AttachCurrentThread(&env_, nullptr);
    config_ = config;
    netplay_.setStatusFn([this](Netplay::State state, int players, const std::string& message) {
        if (!env_ || !bridgeClass || !onNetplayStatusMethod) return;
        jstring msg = env_->NewStringUTF(message.c_str());
        env_->CallStaticVoidMethod(bridgeClass, onNetplayStatusMethod, static_cast<jint>(state),
                                   static_cast<jint>(players), msg);
        env_->DeleteLocalRef(msg);
    });

    std::string error = loadCore(config.corePath);
    if (error.empty()) error = loadGame(config);
    if (!error.empty()) {
        if (core_.handle) unloadCore();
        running_ = false;
        loaded.set_value(error);
        if (vm) vm->DetachCurrentThread();
        env_ = nullptr;
        return;
    }
    audio_.start();
    audio_.prime();
    video_.clear();
    video_.setContentFrameRate(static_cast<float>(avInfo_.timing.fps));
    video_.setActive(true);
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        acceptingCommands_ = true;
    }
    loaded.set_value("");

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    auto lastRun = next;
    int framesSinceFlush = 0;
    bool wasPaused = false;
    float lastSpeed = 1.0f;
    int lastDivisor = 0;
    uint64_t vsyncBase = 0;
    audioPlaying_ = true;
    perfOpen(static_cast<int64_t>(1e9 / (avInfo_.timing.fps > 1.0 ? avInfo_.timing.fps : 60.0)));

    while (running_) {
        runCommands();
        if (paused_) {
            if (!wasPaused) {
                writeSaveFile(RETRO_MEMORY_SAVE_RAM, config_.savePath, lastSram_);
                writeSaveFile(RETRO_MEMORY_RTC, config_.rtcPath, lastRtc_);
                video_.setActive(false);
                wasPaused = true;
            }
            {
                std::unique_lock<std::mutex> lock(cmdMutex_);
                auto wake = [this] { return !paused_ || !running_ || !commands_.empty(); };
                if (netplay_.state() == Netplay::State::Idle) {
                    cmdCv_.wait(lock, wake);
                } else {
                    // Keep answering pings so partners don't drop us while the menu is open.
                    cmdCv_.wait_for(lock, std::chrono::milliseconds(50), wake);
                }
            }
            netplay_.idle();
            next = lastRun = clock::now();
            continue;
        }
        if (wasPaused) {
            audio_.prime();
            video_.setActive(true);
            lastDivisor = 0;
            wasPaused = false;
        }

        const double coreFps = avInfo_.timing.fps > 1.0 ? avInfo_.timing.fps : 60.0;
        const float speed = effectiveSpeed();
        const int divisor = vsyncDivisor(coreFps, speed);
        const bool playAudio = audioEnabled_ && (speed <= 1.0f || speedAudio_);
        if (speed != lastSpeed) {
            // Start the new speed from "now": leaving fast-forward must not leave a backlog of
            // frames to catch up on (a burst followed by stutter).
            next = clock::now();
            lastSpeed = speed;
            // Sound at the old speed is still queued; start the new one cleanly, with headroom
            // for the time-stretcher to get going.
            if (playAudio) audio_.prime();
        } else if (playAudio && !audioPlaying_) {
            audio_.prime();  // e.g. sound was switched back on
        }
        audioPlaying_ = playAudio;
        // Other speeds are time-stretched to real time, keeping the pitch.
        audio_.setTempo(speed);
        // Locked to the display, the game runs at refresh/n instead of its own rate (e.g. 60 vs
        // 59.73 fps); play the sound that much faster so the buffer doesn't drift.
        const double rate = divisor ? video_.refreshRate() / divisor : coreFps * speed;
        audio_.setSpeed(divisor ? rate / coreFps : 1.0);

        auto now = clock::now();
        if (frameTimeCallback_.callback) {
            auto usec = std::chrono::duration_cast<std::chrono::microseconds>(now - lastRun).count();
            frameTimeCallback_.callback(usec > 0 ? usec : frameTimeCallback_.reference);
        }
        lastRun = now;
        netplay_.frame();
        core_.run();
        const int64_t work = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - now).count();
        perfReport(work, static_cast<int64_t>(1e9 / rate));
        emuFrames_++;
        workNs_ += static_cast<uint64_t>(work);

        if (++framesSinceFlush >= 300) {  // ~5 s
            framesSinceFlush = 0;
            writeSaveFile(RETRO_MEMORY_SAVE_RAM, config_.savePath, lastSram_);
            writeSaveFile(RETRO_MEMORY_RTC, config_.rtcPath, lastRtc_);
        }

        if (divisor) {
            // One frame every `divisor` display refreshes: each refresh shows exactly one new frame.
            if (divisor != lastDivisor) vsyncBase = video_.vsyncCount();
            const uint64_t target = vsyncBase + divisor;
            const uint64_t count = video_.vsyncCount();
            if (count >= target) {
                // Late (slow frame or a delayed refresh callback): run the next frame right away
                // so the game keeps its speed, but don't chase a backlog of more than ~4 frames.
                vsyncBase = count - target > 4u * divisor ? count : target;
            } else if (video_.waitForVsync(target, std::chrono::milliseconds(100))) {
                vsyncBase = target;
            } else {
                LOGW("No display refresh for 100 ms; pacing on the clock for a while");
                // No refresh callbacks (display off?): fall back to the clock for a while.
                vsyncSuspendedUntil_ = clock::now() + std::chrono::seconds(1);
            }
            next = clock::now();
        } else {
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / rate));
            now = clock::now();
            if (next < now - std::chrono::milliseconds(50)) {
                next = now;  // fell far behind (slow device or debugger): don't try to catch up
            } else if (netplay_.state() != Netplay::State::Idle) {
                netplay_.waitUntil(next, runSoon_);
            } else {
                std::this_thread::sleep_until(next);
            }
            if (runSoon_.exchange(false)) next = clock::now();
        }
        lastDivisor = divisor;
        pacingVsync_ = divisor != 0;
    }

    video_.setActive(false);
    perfClose();
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        acceptingCommands_ = false;
    }
    runCommands();
    writeSaveFile(RETRO_MEMORY_SAVE_RAM, config_.savePath, lastSram_);
    writeSaveFile(RETRO_MEMORY_RTC, config_.rtcPath, lastRtc_);
    audio_.stop();
    unloadCore();
    if (vm) vm->DetachCurrentThread();
    env_ = nullptr;
}

std::string Frontend::loadCore(const std::string& path) {
    void* handle;
    {
        // A core is dlopen()ed once and kept for the life of the process: cores aren't written to
        // survive dlclose() (static C++ state, thread-locals), but they do support deinit/init.
        std::lock_guard<std::mutex> lock(gCoreHandlesMutex);
        handle = gCoreHandles[path];
        if (!handle) {
            handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) return std::string("코어를 불러올 수 없습니다: ") + dlerror();
            gCoreHandles[path] = handle;
        }
    }

    core_ = {};
    core_.handle = handle;
#define LOAD_SYM(field, name)                                                         \
    core_.field = reinterpret_cast<decltype(core_.field)>(dlsym(handle, #name));     \
    if (!core_.field) return std::string("코어 심볼 누락: ") + #name;
    LOAD_SYM(init, retro_init)
    LOAD_SYM(deinit, retro_deinit)
    LOAD_SYM(api_version, retro_api_version)
    LOAD_SYM(get_system_info, retro_get_system_info)
    LOAD_SYM(get_system_av_info, retro_get_system_av_info)
    LOAD_SYM(set_environment, retro_set_environment)
    LOAD_SYM(set_video_refresh, retro_set_video_refresh)
    LOAD_SYM(set_audio_sample, retro_set_audio_sample)
    LOAD_SYM(set_audio_sample_batch, retro_set_audio_sample_batch)
    LOAD_SYM(set_input_poll, retro_set_input_poll)
    LOAD_SYM(set_input_state, retro_set_input_state)
    LOAD_SYM(set_controller_port_device, retro_set_controller_port_device)
    LOAD_SYM(reset, retro_reset)
    LOAD_SYM(run, retro_run)
    LOAD_SYM(serialize_size, retro_serialize_size)
    LOAD_SYM(serialize, retro_serialize)
    LOAD_SYM(unserialize, retro_unserialize)
    LOAD_SYM(load_game, retro_load_game)
    LOAD_SYM(load_game_special, retro_load_game_special)
    LOAD_SYM(unload_game, retro_unload_game)
    LOAD_SYM(get_memory_data, retro_get_memory_data)
    LOAD_SYM(get_memory_size, retro_get_memory_size)
#undef LOAD_SYM
    core_.cheat_reset = reinterpret_cast<decltype(core_.cheat_reset)>(dlsym(handle, "retro_cheat_reset"));
    core_.cheat_set = reinterpret_cast<decltype(core_.cheat_set)>(dlsym(handle, "retro_cheat_set"));

    {
        std::lock_guard<std::mutex> lock(optionsMutex_);
        optionDefaults_.clear();
        optionValues_.clear();
        optionOverrides_ = config_.options;
    }
    subsystems_.clear();
    hasNetpacket_ = false;
    pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    frameTimeCallback_ = {};
    avInfo_ = {};

    core_.set_environment(envTrampoline);
    core_.set_video_refresh(videoTrampoline);
    core_.set_audio_sample(audioSampleTrampoline);
    core_.set_audio_sample_batch(audioBatchTrampoline);
    core_.set_input_poll(inputPollTrampoline);
    core_.set_input_state(inputStateTrampoline);
    core_.init();
    return "";
}

std::string Frontend::loadGame(const LaunchConfig& config) {
    contentData_.clear();
    contentPaths_.clear();
    contentData_.reserve(3);
    contentPaths_.reserve(3);

    auto addContent = [this](const std::string& path, bool withData) -> retro_game_info {
        contentPaths_.push_back(path);
        contentData_.emplace_back();
        retro_game_info info{};
        info.path = contentPaths_.back().c_str();
        if (withData && contentData_.back().map(path)) {
            info.data = contentData_.back().data;
            info.size = contentData_.back().size;
        }
        return info;
    };

    bool ok;
    if (!config.slot2RomPath.empty()) {
        bool haveSave = !config.slot2SavePath.empty() && fileExists(config.slot2SavePath);
        const char* ident = haveSave ? "gba" : "gbanosav";
        auto it = std::find_if(subsystems_.begin(), subsystems_.end(),
                               [&](const Subsystem& s) { return s.ident == ident; });
        if (it == subsystems_.end()) return "이 코어는 GBA 슬롯(Slot-2)을 지원하지 않습니다.";

        retro_game_info infos[3];
        infos[0] = addContent(config.romPath, true);
        infos[1] = addContent(config.slot2RomPath, true);
        size_t count = 2;
        if (haveSave) infos[count++] = addContent(config.slot2SavePath, false);
        if (!infos[0].data || !infos[1].data) return "ROM 파일을 읽을 수 없습니다.";
        ok = core_.load_game_special(it->id, infos, count);
    } else {
        retro_game_info info = addContent(config.romPath, true);
        if (!info.data) return "ROM 파일을 읽을 수 없습니다.";
        ok = core_.load_game(&info);
    }
    if (!ok) return "게임을 불러오지 못했습니다. 지원하지 않는 ROM일 수 있습니다.";

    retro_system_info sysInfo{};
    core_.get_system_info(&sysInfo);
    std::string version = std::string(sysInfo.library_name ? sysInfo.library_name : "") + " " +
                          (sysInfo.library_version ? sysInfo.library_version : "");
    netplay_.setCoreCallbacks(hasNetpacket_ ? &netpacketCallbacks_ : nullptr, version);

    core_.get_system_av_info(&avInfo_);
    audio_.setInputRate(avInfo_.timing.sample_rate);
    core_.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    LOGI("Loaded %s: %ux%u @ %.3f fps, audio %.0f Hz", config.romPath.c_str(), avInfo_.geometry.base_width,
         avInfo_.geometry.base_height, avInfo_.timing.fps, avInfo_.timing.sample_rate);

    lastSram_.clear();
    lastRtc_.clear();
    loadSaveFile(RETRO_MEMORY_SAVE_RAM, config.savePath);
    loadSaveFile(RETRO_MEMORY_RTC, config.rtcPath);
    return "";
}

void Frontend::unloadCore() {
    if (!core_.handle) return;
    setRumble(0, RETRO_RUMBLE_STRONG, 0);
    setRumble(0, RETRO_RUMBLE_WEAK, 0);
    netplay_.clearCoreCallbacks();
    core_.unload_game();
    core_.deinit();
    core_ = {};
    contentData_.clear();
    contentPaths_.clear();
}

// ---- commands -------------------------------------------------------------------------------

void Frontend::runCommands() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        pending.swap(commands_);
    }
    for (auto& cmd : pending) cmd();
}

bool Frontend::runOnEmuThread(std::function<bool()> fn) {
    if (std::this_thread::get_id() == thread_.get_id()) return fn();
    auto task = std::make_shared<std::packaged_task<bool()>>(std::move(fn));
    std::future<bool> result = task->get_future();
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        if (!acceptingCommands_) return false;
        commands_.push_back([task] { (*task)(); });
    }
    cmdCv_.notify_all();
    // The thread stops accepting commands before its final drain, so this can't hang.
    return result.get();
}

bool Frontend::saveState(const std::string& path) {
    return runOnEmuThread([this, path] {
        size_t size = core_.serialize_size();
        if (size == 0) return false;
        std::vector<uint8_t> buffer(size);
        if (!core_.serialize(buffer.data(), size)) return false;
        return writeFileAtomic(path, buffer.data(), size);
    });
}

bool Frontend::loadState(const std::string& path) {
    return runOnEmuThread([this, path] {
        if (netplay_.active()) return false;  // would desync the other consoles
        std::vector<uint8_t> buffer;
        if (!readFile(path, buffer) || buffer.empty()) return false;
        bool ok = core_.unserialize(buffer.data(), buffer.size());
        audio_.prime();
        return ok;
    });
}

std::string Frontend::netplayHost(const std::string& title, const std::string& tag) {
    auto error = std::make_shared<std::string>("게임이 실행 중이 아닙니다.");
    runOnEmuThread([this, title, tag, error] {
        netplay_.setTitle(title);
        netplay_.setTag(tag);
        *error = netplay_.host();
        return error->empty();
    });
    return *error;
}

std::string Frontend::netplayJoin(const std::string& address, const std::string& tag) {
    auto error = std::make_shared<std::string>("게임이 실행 중이 아닙니다.");
    runOnEmuThread([this, address, tag, error] {
        netplay_.setTag(tag);
        *error = netplay_.join(address);
        return error->empty();
    });
    return *error;
}

void Frontend::netplayLeave() {
    runOnEmuThread([this] {
        netplay_.leave("무선 통신을 종료했습니다.");
        return true;
    });
}

void Frontend::reset() {
    runOnEmuThread([this] {
        core_.reset();
        return true;
    });
}

void Frontend::setCheats(std::vector<std::string> codes) {
    runOnEmuThread([this, codes = std::move(codes)] {
        if (!core_.cheat_reset || !core_.cheat_set) return false;
        // Cores differ in whether "disabled" means anything (mGBA ignores it), so only enabled
        // cheats are ever passed in, after clearing the previous set.
        core_.cheat_reset();
        for (size_t i = 0; i < codes.size(); i++) {
            core_.cheat_set(static_cast<unsigned>(i), true, codes[i].c_str());
        }
        return true;
    });
}

void Frontend::flushSaves() {
    runOnEmuThread([this] {
        writeSaveFile(RETRO_MEMORY_SAVE_RAM, config_.savePath, lastSram_);
        writeSaveFile(RETRO_MEMORY_RTC, config_.rtcPath, lastRtc_);
        return true;
    });
}

void Frontend::setOption(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(optionsMutex_);
    optionOverrides_[key] = value;
    config_.options[key] = value;
    optionsDirty_ = true;
}

// ---- battery saves --------------------------------------------------------------------------

void Frontend::loadSaveFile(unsigned memoryId, const std::string& path) {
    if (path.empty()) return;
    void* ptr = core_.get_memory_data(memoryId);
    size_t size = core_.get_memory_size(memoryId);
    if (!ptr || size == 0) return;
    std::vector<uint8_t> data;
    if (!readFile(path, data)) return;
    memcpy(ptr, data.data(), std::min(size, data.size()));
    auto& last = memoryId == RETRO_MEMORY_RTC ? lastRtc_ : lastSram_;
    last.assign(static_cast<uint8_t*>(ptr), static_cast<uint8_t*>(ptr) + size);
    LOGI("Loaded %zu-byte save (memory %u) from %s", data.size(), memoryId, path.c_str());
}

void Frontend::writeSaveFile(unsigned memoryId, const std::string& path, std::vector<uint8_t>& lastWritten) {
    if (path.empty() || !core_.handle) return;
    auto* ptr = static_cast<uint8_t*>(core_.get_memory_data(memoryId));
    size_t size = core_.get_memory_size(memoryId);
    if (!ptr || size == 0) return;
    if (lastWritten.size() == size && memcmp(lastWritten.data(), ptr, size) == 0) return;
    if (lastWritten.empty() && !fileExists(path) &&
        std::all_of(ptr, ptr + size, [](uint8_t b) { return b == 0xFF || b == 0x00; })) {
        return;  // nothing has been saved yet; don't create a blank save file
    }
    if (writeFileAtomic(path, ptr, size)) {
        lastWritten.assign(ptr, ptr + size);
        LOGI("Wrote %zu-byte save (memory %u) to %s", size, memoryId, path.c_str());
    } else {
        LOGE("Failed to write save %s", path.c_str());
    }
}

// ---- video / pacing ------------------------------------------------------------------------

void Frontend::videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0) return;

    if (width != frameWidth_ || height != frameHeight_) {
        frameWidth_ = width;
        frameHeight_ = height;
        if (env_ && bridgeClass && onVideoSizeMethod) {
            env_->CallStaticVoidMethod(bridgeClass, onVideoSizeMethod, static_cast<jint>(width),
                                       static_cast<jint>(height));
        }
    }
    // Fast-forward makes frames faster than the display can show; only hand over the ones it will.
    video_.submit(data, width, height, pitch, pixelFormat_, effectiveSpeed() > 1.0f);
}

int Frontend::vsyncDivisor(double coreFps, float speed) const {
    if (!vsyncPacing_ || speed != 1.0f || netplay_.state() != Netplay::State::Idle) return 0;
    if (std::chrono::steady_clock::now() < vsyncSuspendedUntil_) return 0;
    const double hz = video_.refreshRate();
    if (hz < 20.0) return 0;
    const long n = std::lround(hz / coreFps);
    if (n < 1 || n > 4) return 0;
    // Within 1% (e.g. 59.73 or 59.83 fps on a 60/120 Hz display). Otherwise (90 Hz, 144 Hz...) a
    // steady clock beats running visibly too fast or too slow.
    if (std::fabs(hz / static_cast<double>(n) - coreFps) / coreFps > 0.01) return 0;
    return static_cast<int>(n);
}

void Frontend::stats(float out[6]) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    auto now = std::chrono::steady_clock::now();
    uint64_t frames = emuFrames_.load(), presented = video_.presentedFrames(), work = workNs_.load();
    double dt = std::chrono::duration<double>(now - statsTime_).count();
    bool valid = statsTime_.time_since_epoch().count() != 0 && dt > 0.05 && frames >= statsFrames_;
    out[0] = valid ? static_cast<float>((frames - statsFrames_) / dt) : 0.0f;
    out[1] = valid ? static_cast<float>((presented - statsPresented_) / dt) : 0.0f;
    out[2] = static_cast<float>(video_.refreshRate());
    out[3] = valid && frames > statsFrames_
                 ? static_cast<float>((work - statsWork_) / 1e6 / static_cast<double>(frames - statsFrames_))
                 : 0.0f;
    out[4] = pacingVsync_ ? 1.0f : 0.0f;
    out[5] = effectiveSpeed();
    statsTime_ = now;
    statsFrames_ = frames;
    statsPresented_ = presented;
    statsWork_ = work;
}

// APerformanceHint (libandroid, API 33) is looked up at runtime since minSdk is 26.
namespace {
struct PerfHintApi {
    void* (*getManager)() = nullptr;
    void* (*createSession)(void*, const int32_t*, size_t, int64_t) = nullptr;
    int (*updateTarget)(void*, int64_t) = nullptr;
    int (*reportActual)(void*, int64_t) = nullptr;
    void (*closeSession)(void*) = nullptr;
    PerfHintApi() {
        void* android = dlopen("libandroid.so", RTLD_NOW);
        if (!android) return;
        getManager = reinterpret_cast<decltype(getManager)>(dlsym(android, "APerformanceHint_getManager"));
        createSession = reinterpret_cast<decltype(createSession)>(dlsym(android, "APerformanceHint_createSession"));
        updateTarget = reinterpret_cast<decltype(updateTarget)>(dlsym(android, "APerformanceHint_updateTargetWorkDuration"));
        reportActual = reinterpret_cast<decltype(reportActual)>(dlsym(android, "APerformanceHint_reportActualWorkDuration"));
        closeSession = reinterpret_cast<decltype(closeSession)>(dlsym(android, "APerformanceHint_closeSession"));
    }
    bool available() const { return getManager && createSession && updateTarget && reportActual && closeSession; }
};

const PerfHintApi& perfHintApi() {
    static PerfHintApi api;
    return api;
}
}  // namespace

void Frontend::perfOpen(int64_t targetNs) {
    const auto& api = perfHintApi();
    if (perfSession_ || !api.available()) return;
    void* manager = api.getManager();
    if (!manager) return;
    int32_t tid = gettid();
    perfSession_ = api.createSession(manager, &tid, 1, targetNs);
    perfTarget_ = targetNs;
    LOGI("Performance hint session: %s", perfSession_ ? "on" : "unavailable");
}

void Frontend::perfReport(int64_t actualNs, int64_t targetNs) {
    if (!perfSession_) return;
    const auto& api = perfHintApi();
    if (targetNs > 0 && targetNs != perfTarget_) {
        api.updateTarget(perfSession_, targetNs);
        perfTarget_ = targetNs;
    }
    api.reportActual(perfSession_, actualNs > 0 ? actualNs : 1);
}

void Frontend::perfClose() {
    if (!perfSession_) return;
    perfHintApi().closeSession(perfSession_);
    perfSession_ = nullptr;
}

// ---- audio / input --------------------------------------------------------------------------

size_t Frontend::audioBatch(const int16_t* data, size_t frames) {
    if (audioPlaying_) audio_.push(data, frames);
    return frames;
}

void Frontend::setRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    if (port != 0 || effect > RETRO_RUMBLE_WEAK) return;
    rumble_[effect] = strength;
    int level = std::max(rumble_[0], rumble_[1]) >> 8;  // 0..255
    if (level == lastRumble_ || !env_ || !bridgeClass || !onRumbleMethod) return;
    lastRumble_ = level;
    env_->CallStaticVoidMethod(bridgeClass, onRumbleMethod, static_cast<jint>(level));
}

void Frontend::setPointer(int x, int y, bool pressed) {
    pointerX_ = x;
    pointerY_ = y;
    pointerPressed_ = pressed;
}

int16_t Frontend::inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    switch (device & RETRO_DEVICE_MASK) {
        case RETRO_DEVICE_JOYPAD: {
            uint32_t mask = buttons_;
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(mask & 0xFFFF);
            return id < 16 ? static_cast<int16_t>((mask >> id) & 1) : 0;
        }
        case RETRO_DEVICE_POINTER:
            if (index != 0) return 0;
            switch (id) {
                case RETRO_DEVICE_ID_POINTER_X: return static_cast<int16_t>(pointerX_.load());
                case RETRO_DEVICE_ID_POINTER_Y: return static_cast<int16_t>(pointerY_.load());
                case RETRO_DEVICE_ID_POINTER_PRESSED: return pointerPressed_ ? 1 : 0;
                case RETRO_DEVICE_ID_POINTER_COUNT: return pointerPressed_ ? 1 : 0;
                default: return 0;
            }
        default:
            return 0;
    }
}

// ---- environment ----------------------------------------------------------------------------

void Frontend::registerOptionDefault(const char* key, const char* value) {
    if (!key) return;
    optionDefaults_[key] = value ? value : "";
}

void Frontend::postMessage(const char* message) {
    if (!message || !*message || !env_ || !bridgeClass || !onMessageMethod) return;
    // Some cores re-post the same OSD message every frame; only surface it once in a while.
    auto now = std::chrono::steady_clock::now();
    if (lastMessage_ == message && now - lastMessageTime_ < std::chrono::seconds(5)) return;
    lastMessage_ = message;
    lastMessageTime_ = now;
    LOGI("Core message: %s", message);
    jstring str = env_->NewStringUTF(message);
    env_->CallStaticVoidMethod(bridgeClass, onMessageMethod, str);
    env_->DeleteLocalRef(str);
}

bool Frontend::environment(unsigned cmd, void* data) {
    switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            auto format = *static_cast<const retro_pixel_format*>(data);
            if (format > RETRO_PIXEL_FORMAT_RGB565) return false;
            pixelFormat_ = format;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char**>(data) = config_.systemDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = config_.saveDir.c_str();
            return true;

        case RETRO_ENVIRONMENT_GET_USERNAME:
            *static_cast<const char**>(data) = "Player";  // DS firmware nickname
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = config_.language;
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = coreLog;
            return true;

        // -- core options --
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *static_cast<unsigned*>(data) = 2;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const retro_core_options_v2* options =
                (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2
                    ? static_cast<const retro_core_options_v2*>(data)
                    : static_cast<const retro_core_options_v2_intl*>(data)->us;
            if (!options || !options->definitions) return true;
            std::lock_guard<std::mutex> lock(optionsMutex_);
            for (auto* def = options->definitions; def->key; def++) {
                registerOptionDefault(def->key, def->default_value ? def->default_value : def->values[0].value);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            const retro_core_option_definition* defs =
                (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) == RETRO_ENVIRONMENT_SET_CORE_OPTIONS
                    ? static_cast<const retro_core_option_definition*>(data)
                    : static_cast<const retro_core_options_intl*>(data)->us;
            if (!defs) return true;
            std::lock_guard<std::mutex> lock(optionsMutex_);
            for (auto* def = defs; def->key; def++) {
                registerOptionDefault(def->key, def->default_value ? def->default_value : def->values[0].value);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            std::lock_guard<std::mutex> lock(optionsMutex_);
            for (auto* var = static_cast<const retro_variable*>(data); var && var->key; var++) {
                // "Description; first|second|third" -> default is "first"
                std::string spec = var->value ? var->value : "";
                size_t semi = spec.find("; ");
                std::string values = semi == std::string::npos ? spec : spec.substr(semi + 2);
                registerOptionDefault(var->key, values.substr(0, values.find('|')).c_str());
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* var = static_cast<retro_variable*>(data);
            if (!var || !var->key) return false;
            std::lock_guard<std::mutex> lock(optionsMutex_);
            auto over = optionOverrides_.find(var->key);
            if (over != optionOverrides_.end()) {
                var->value = (optionValues_[var->key] = over->second).c_str();
                return true;
            }
            auto def = optionDefaults_.find(var->key);
            if (def != optionDefaults_.end()) {
                var->value = (optionValues_[var->key] = def->second).c_str();
                return true;
            }
            var->value = nullptr;
            return false;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            auto* var = static_cast<const retro_variable*>(data);
            if (!var) return true;  // query: setting variables is supported
            if (!var->key || !var->value) return false;
            std::lock_guard<std::mutex> lock(optionsMutex_);
            optionOverrides_[var->key] = var->value;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = optionsDirty_.exchange(false);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
            return true;

        // -- content / subsystems --
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: {
            subsystems_.clear();
            for (auto* info = static_cast<const retro_subsystem_info*>(data); info && info->ident; info++) {
                subsystems_.push_back({info->ident, info->id});
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
            return true;

        // -- timing / AV --
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            avInfo_ = *static_cast<const retro_system_av_info*>(data);
            audio_.setInputRate(avInfo_.timing.sample_rate);
            video_.setContentFrameRate(static_cast<float>(avInfo_.timing.fps));
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            avInfo_.geometry = *static_cast<const retro_game_geometry*>(data);
            return true;
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
            frameTimeCallback_ = *static_cast<const retro_frame_time_callback*>(data);
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *static_cast<bool*>(data) = effectiveSpeed() > 1.0f;
            return true;
        case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
            auto* state = static_cast<retro_throttle_state*>(data);
            const float speed = effectiveSpeed();
            state->mode = speed > 1.0f   ? RETRO_THROTTLE_FAST_FORWARD
                          : speed < 1.0f ? RETRO_THROTTLE_SLOW_MOTION
                          : pacingVsync_ ? RETRO_THROTTLE_VSYNC
                                         : RETRO_THROTTLE_NONE;
            state->rate = static_cast<float>(avInfo_.timing.fps * speed);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            *static_cast<float*>(data) = 60.0f;
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *static_cast<int*>(data) = 3;
            return true;

        // -- input --
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
            *static_cast<uint64_t*>(data) = (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_POINTER);
            return true;

        // -- messages --
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *static_cast<unsigned*>(data) = 1;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            postMessage(static_cast<const retro_message*>(data)->msg);
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            auto* msg = static_cast<const retro_message_ext*>(data);
            if (msg->target != RETRO_MESSAGE_TARGET_LOG && msg->type != RETRO_MESSAGE_TYPE_PROGRESS) {
                postMessage(msg->msg);
            } else {
                LOGI("Core: %s", msg->msg);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            static_cast<retro_rumble_interface*>(data)->set_rumble_state = rumbleTrampoline;
            return true;

        case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
            hasNetpacket_ = data != nullptr;
            if (data) netpacketCallbacks_ = *static_cast<const retro_netpacket_callback*>(data);
            return true;

        case RETRO_ENVIRONMENT_PRIVATE | 0x4D4C:  // RETRO_ENVIRONMENT_LINK_RUN_SOON (cores/mgba-link)
            runSoon_ = true;
            return true;

        case RETRO_ENVIRONMENT_SHUTDOWN:
            running_ = false;
            return true;

        default:
            // Unsupported: HW rendering, VFS, rumble, sensors, microphone...
            return false;
    }
}
