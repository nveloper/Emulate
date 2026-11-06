package com.emulate.app

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class Cheat(val name: String, val code: String, val enabled: Boolean)

/** Per-game cheat list, stored as JSON in files/cheats/<rom file>.json. */
class CheatStore(private val file: File) {

    fun load(): List<Cheat> {
        if (!file.isFile) return emptyList()
        return runCatching {
            val array = JSONArray(file.readText())
            (0 until array.length()).map { i ->
                val o = array.getJSONObject(i)
                Cheat(o.getString("name"), o.getString("code"), o.optBoolean("enabled", true))
            }
        }.getOrDefault(emptyList())
    }

    fun save(cheats: List<Cheat>) {
        file.parentFile?.mkdirs()
        val array = JSONArray()
        cheats.forEach { array.put(JSONObject().put("name", it.name).put("code", it.code).put("enabled", it.enabled)) }
        file.writeText(array.toString(2))
    }

    companion object {
        /** Uppercase hex/Game Genie tokens separated by single spaces, as both cores accept. */
        fun normalize(code: String): String =
            code.uppercase().split(Regex("[\\s+]+")).filter { it.isNotEmpty() }.joinToString(" ")

        /** Cheap sanity check per platform; the cores do the real parsing. */
        fun looksValid(platform: Platform, code: String): Boolean {
            val tokens = normalize(code).split(' ').filter { it.isNotEmpty() }
            if (tokens.isEmpty()) return false
            val hex = Regex("[0-9A-F]+")
            return when (platform) {
                // GameShark (8 hex digits) or Game Genie (XXX-XXX[-XXX])
                Platform.GB, Platform.GBC -> tokens.all { (it.length == 8 && hex.matches(it)) || Regex("[0-9A-F]{3}-[0-9A-F]{3}(-[0-9A-F]{3})?").matches(it) }
                // GameShark / Action Replay (8+8) or CodeBreaker (8+4) pairs
                Platform.GBA -> tokens.all { hex.matches(it) && (it.length == 8 || it.length == 4) }
                // Action Replay DS: 8+8 hex pairs
                Platform.NDS -> tokens.size % 2 == 0 && tokens.all { it.length == 8 && hex.matches(it) }
            }
        }

        fun example(platform: Platform) = when (platform) {
            Platform.GB, Platform.GBC -> "예: 010A62D0  또는  0A9-62F-E66"
            Platform.GBA -> "예: 82025840 0063\n(여러 줄이면 줄마다 하나씩)"
            Platform.NDS -> "예: 221C6E10 00000063\nD2000000 00000000"
        }
    }
}
