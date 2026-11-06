package com.emulate.app

import android.content.Context
import org.json.JSONObject

/** Parts of the on-screen gamepad that can be moved, resized or hidden as a unit. */
enum class TouchElement(val label: String) {
    DPAD("방향키"),
    FACE("A·B 버튼"),
    L("L"),
    R("R"),
    SELECT("SELECT"),
    START("START"),
    MIC("MIC"),
    MENU("메뉴 (≡)"),
    FAST_FORWARD("빨리 감기 (▶▶)"),
    TOGGLE("버튼 숨기기 토글"),
}

/**
 * A user placement: centre as a fraction of the overlay's width/height, a size multiplier and
 * whether the element is hidden. Elements without one use the built-in layout.
 */
data class TouchPlacement(val x: Float, val y: Float, val scale: Float = 1f, val hidden: Boolean = false)

/**
 * Touch layouts in SharedPreferences "touch_layout", one per controller shape (GB/GBC share one)
 * and orientation.
 */
class TouchLayoutStore(context: Context) {
    private val prefs = context.getSharedPreferences("touch_layout", Context.MODE_PRIVATE)

    fun load(platform: Platform, landscape: Boolean): Map<TouchElement, TouchPlacement> {
        val json = prefs.getString(key(platform, landscape), null) ?: return emptyMap()
        return runCatching {
            val obj = JSONObject(json)
            buildMap {
                for (element in TouchElement.entries) {
                    val p = obj.optJSONObject(element.name) ?: continue
                    put(
                        element,
                        TouchPlacement(
                            x = p.getDouble("x").toFloat(),
                            y = p.getDouble("y").toFloat(),
                            scale = p.optDouble("s", 1.0).toFloat(),
                            hidden = p.optBoolean("h", false),
                        ),
                    )
                }
            }
        }.getOrDefault(emptyMap())
    }

    fun save(platform: Platform, landscape: Boolean, placements: Map<TouchElement, TouchPlacement>) {
        if (placements.isEmpty()) {
            prefs.edit().remove(key(platform, landscape)).apply()
            return
        }
        val obj = JSONObject()
        placements.forEach { (element, p) ->
            obj.put(element.name, JSONObject().put("x", p.x.toDouble()).put("y", p.y.toDouble())
                .put("s", p.scale.toDouble()).put("h", p.hidden))
        }
        prefs.edit().putString(key(platform, landscape), obj.toString()).apply()
    }

    fun isEmpty(): Boolean = prefs.all.isEmpty()

    fun clearAll() = prefs.edit().clear().apply()

    private fun key(platform: Platform, landscape: Boolean): String {
        val shape = when (platform) {
            Platform.GB, Platform.GBC -> "gb"
            Platform.GBA -> "gba"
            Platform.NDS -> "nds"
        }
        return "${shape}_${if (landscape) "land" else "port"}"
    }
}
