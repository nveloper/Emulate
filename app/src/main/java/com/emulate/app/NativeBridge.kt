package com.emulate.app

import android.view.Surface

/** JNI surface of the native libretro frontend (app/src/main/cpp). One game runs at a time. */
object NativeBridge {
    init {
        System.loadLibrary("emufrontend")
    }

    interface Listener {
        /** Called on the emulation thread when the core's output resolution changes. */
        fun onVideoSize(width: Int, height: Int)

        /** Called on the emulation thread for user-facing core messages. */
        fun onMessage(message: String)

        /** Called on the emulation thread when the local wireless session changes. */
        fun onNetplayStatus(state: Int, players: Int, message: String) {}

        /** Called on the emulation thread: rumble strength 0 (off) to 255. */
        fun onRumble(level: Int) {}
    }

    @Volatile
    var listener: Listener? = null

    @JvmStatic
    fun onVideoSize(width: Int, height: Int) {
        listener?.onVideoSize(width, height)
    }

    @JvmStatic
    fun onMessage(message: String) {
        listener?.onMessage(message)
    }

    @JvmStatic
    fun onNetplayStatus(state: Int, players: Int, message: String) {
        listener?.onNetplayStatus(state, players, message)
    }

    @JvmStatic
    fun onRumble(level: Int) {
        listener?.onRumble(level)
    }

    /** Loads the core and game and starts emulating. Returns null on success, else an error message. */
    external fun start(
        corePath: String,
        romPath: String,
        savePath: String,
        rtcPath: String,
        systemDir: String,
        saveDir: String,
        slot2RomPath: String?,
        slot2SavePath: String?,
        optionKeys: Array<String>,
        optionValues: Array<String>,
        language: Int,
    ): String?

    /** Device language as a libretro RETRO_LANGUAGE_* value (used e.g. for the DS firmware language). */
    fun retroLanguage(locale: java.util.Locale = java.util.Locale.getDefault()): Int = when (locale.language) {
        "ja" -> 1
        "fr" -> 2
        "es" -> 3
        "de" -> 4
        "it" -> 5
        "nl" -> 6
        "pt" -> if (locale.country == "BR") 7 else 8
        "ru" -> 9
        "ko" -> 10
        "zh" -> if (locale.script == "Hant" || locale.country in setOf("TW", "HK", "MO")) 11 else 12
        else -> 0
    }

    external fun stop()
    external fun setPaused(paused: Boolean)
    /** 1 = normal, >1 fast-forward, <1 slow motion (forced to 1 during netplay). */
    external fun setSpeed(speed: Float)
    /** Keep sound (time-stretched, same pitch) while fast-forwarding. */
    external fun setSpeedAudio(enabled: Boolean)
    /** Pace frames on the display refresh when it matches the game's frame rate. */
    external fun setVsyncPacing(enabled: Boolean)
    /** One of [Settings.VIDEO_FILTERS]' indices. */
    external fun setVideoFilter(filter: Int)
    /** [emulated fps, displayed fps, display Hz, core ms per frame, vsync-paced (0/1), speed] */
    external fun getStats(): FloatArray
    /** Where the game picture goes on the full-screen surface, in pixels. */
    external fun setGameRect(x: Int, y: Int, width: Int, height: Int)
    /** Soft glow of the picture's edge colours around it (behind the controls). */
    external fun setAmbient(enabled: Boolean)
    /** The newest frame as [width, height, ARGB pixels...], or null before the first frame. */
    external fun captureFrame(): IntArray?
    external fun setAudioEnabled(enabled: Boolean)
    external fun setSurface(surface: Surface?)
    external fun setViewSize(width: Int, height: Int)
    external fun setButtons(mask: Int)
    external fun setPointer(x: Int, y: Int, pressed: Boolean)
    external fun setOption(key: String, value: String)
    external fun saveState(path: String): Boolean
    external fun loadState(path: String): Boolean
    external fun reset()
    external fun flushSaves()
    external fun setCheats(codes: Array<String>)

    /** NDS local wireless / GB-GBA link cable. Each returns null on success or a user-facing error. */
    external fun netplayHost(title: String, tag: String): String?
    external fun netplayJoin(address: String, tag: String): String?
    external fun netplayLeave()
}

/** libretro RETRO_DEVICE_ID_JOYPAD_* bit positions. */
object Pad {
    const val B = 1 shl 0
    const val Y = 1 shl 1
    const val SELECT = 1 shl 2
    const val START = 1 shl 3
    const val UP = 1 shl 4
    const val DOWN = 1 shl 5
    const val LEFT = 1 shl 6
    const val RIGHT = 1 shl 7
    const val A = 1 shl 8
    const val X = 1 shl 9
    const val L = 1 shl 10
    const val R = 1 shl 11
    const val L2 = 1 shl 12
    const val L3 = 1 shl 14  // melonDS DS: microphone
}
