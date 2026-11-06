package com.emulate.app

import android.content.Context

/** User preferences (SharedPreferences "settings"). */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    /** On-screen controls opacity, 0.1 .. 1. */
    var controlsOpacity: Float
        get() = prefs.getFloat("controls_opacity", 0.8f)
        set(value) = prefs.edit().putFloat("controls_opacity", value).apply()

    /** On-screen controls size multiplier, 0.7 .. 1.4. */
    var controlsScale: Float
        get() = prefs.getFloat("controls_scale", 1f)
        set(value) = prefs.edit().putFloat("controls_scale", value).apply()

    var haptics: Boolean
        get() = prefs.getBoolean("haptics", true)
        set(value) = prefs.edit().putBoolean("haptics", value).apply()

    var rumble: Boolean
        get() = prefs.getBoolean("rumble", true)
        set(value) = prefs.edit().putBoolean("rumble", value).apply()

    var audio: Boolean
        get() = prefs.getBoolean("audio", true)
        set(value) = prefs.edit().putBoolean("audio", value).apply()

    /** Speed the fast-forward button switches to. */
    var fastForwardSpeed: Float
        get() = prefs.getFloat("ff_speed", 2f)
        set(value) = prefs.edit().putFloat("ff_speed", value).apply()

    /** Fast-forward only while the button is held (instead of tap to toggle). */
    var fastForwardHold: Boolean
        get() = prefs.getBoolean("ff_hold", false)
        set(value) = prefs.edit().putBoolean("ff_hold", value).apply()

    /**
     * Keep the sound while fast-forwarding (time-stretched, same pitch). A new key: it used to be
     * off by default, and sound at every speed is what players expect.
     */
    var fastForwardAudio: Boolean
        get() = prefs.getBoolean("speed_audio", true)
        set(value) = prefs.edit().putBoolean("speed_audio", value).apply()

    /** One of [VIDEO_FILTERS]' keys. */
    var videoFilter: String
        get() = prefs.getString("video_filter", "sharp") ?: "sharp"
        set(value) = prefs.edit().putString("video_filter", value).apply()

    val videoFilterIndex: Int get() = VIDEO_FILTERS.keys.indexOf(videoFilter).coerceAtLeast(0)

    /** Pace emulation on the display refresh when the rates match (smoothest motion). */
    var vsync: Boolean
        get() = prefs.getBoolean("vsync", true)
        set(value) = prefs.edit().putBoolean("vsync", value).apply()

    /** Glow of the game's edge colours behind the touch controls (like YouTube's ambient mode). */
    var ambient: Boolean
        get() = prefs.getBoolean("ambient", true)
        set(value) = prefs.edit().putBoolean("ambient", value).apply()

    /** Show frame rate / timing in the corner while playing. */
    var showStats: Boolean
        get() = prefs.getBoolean("show_stats", false)
        set(value) = prefs.edit().putBoolean("show_stats", value).apply()

    /** GB (not GBC) games: one of [GB_PALETTES]' keys. */
    var gbPalette: String
        get() = prefs.getString("gb_palette", "auto") ?: "auto"
        set(value) = prefs.edit().putString("gb_palette", value).apply()

    /** NDS screen layout (melonDS DS value) per orientation. */
    fun ndsLayout(landscape: Boolean): String =
        prefs.getString(if (landscape) "nds_layout_land" else "nds_layout_port", null)
            ?: if (landscape) "left-right" else "top-bottom"

    fun setNdsLayout(landscape: Boolean, value: String) =
        prefs.edit().putString(if (landscape) "nds_layout_land" else "nds_layout_port", value).apply()

    /** Core options derived from these settings. */
    fun coreOptions(platform: Platform): Map<String, String> = when (platform) {
        Platform.GB, Platform.GBC, Platform.GBA -> when (val palette = gbPalette) {
            // Like a real Game Boy Color: original GB games get the GBC boot ROM's palette for them.
            "auto" -> mapOf("mgba_gb_colors_preset" to "1")
            else -> mapOf("mgba_gb_colors_preset" to "0", "mgba_gb_colors" to palette)
        }
        Platform.NDS -> emptyMap()
    }

    companion object {
        val GB_PALETTES = linkedMapOf(
            "auto" to "GBC처럼 자동 색칠",
            "Grayscale" to "흑백",
            "DMG Green" to "초록 (원조 게임보이)",
            "GB Pocket" to "게임보이 포켓",
            "GB Light" to "게임보이 라이트",
        )

        val NDS_LAYOUTS = linkedMapOf(
            "top-bottom" to "위/아래",
            "bottom-top" to "위/아래 (화면 바꿈)",
            "left-right" to "좌/우",
            "right-left" to "좌/우 (화면 바꿈)",
            "hybrid-bottom" to "아래 화면 크게",
            "hybrid-top" to "위 화면 크게",
            "bottom" to "아래 화면만",
            "top" to "위 화면만",
        )

        /** Order matches the native VideoOutput::Filter values. */
        val VIDEO_FILTERS = linkedMapOf(
            "sharp" to "선명하게 (권장)",
            "nearest" to "원본 픽셀",
            "smooth" to "부드럽게",
            "lcd" to "LCD 격자",
        )

        val FAST_FORWARD_SPEEDS = listOf(1.5f, 2f, 3f, 4f, 6f, 8f)

        /** Choices in the in-game speed menu. */
        val GAME_SPEEDS = listOf(0.5f, 0.75f, 1f, 1.5f, 2f, 3f, 4f, 6f, 8f)

        fun speedLabel(speed: Float): String =
            if (speed == speed.toInt().toFloat()) "×${speed.toInt()}" else "×$speed"
    }
}
