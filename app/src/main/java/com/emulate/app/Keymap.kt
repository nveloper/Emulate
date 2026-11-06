package com.emulate.app

import android.content.Context
import android.view.KeyEvent

/** Things a physical controller / keyboard key can do. */
enum class PadAction(val label: String, val bit: Int) {
    UP("위", Pad.UP),
    DOWN("아래", Pad.DOWN),
    LEFT("왼쪽", Pad.LEFT),
    RIGHT("오른쪽", Pad.RIGHT),
    A("A", Pad.A),
    B("B", Pad.B),
    X("X (NDS)", Pad.X),
    Y("Y (NDS)", Pad.Y),
    L("L", Pad.L),
    R("R", Pad.R),
    START("START", Pad.START),
    SELECT("SELECT", Pad.SELECT),
    MENU("메뉴 열기", 0),
    FAST_FORWARD("빨리 감기 켜기/끄기", 0),
    QUICK_SAVE("빠른 저장 (슬롯 1)", 0),
    QUICK_LOAD("빠른 불러오기 (슬롯 1)", 0),
}

/** Key code -> action, stored in SharedPreferences "keymap" (one entry per action). */
class Keymap(context: Context) {
    private val prefs = context.getSharedPreferences("keymap", Context.MODE_PRIVATE)

    fun keyFor(action: PadAction): Int = prefs.getInt(action.name, DEFAULTS[action] ?: KeyEvent.KEYCODE_UNKNOWN)

    /** Assigns [keyCode] to [action], taking it away from any other action that had it. */
    fun assign(action: PadAction, keyCode: Int) {
        val editor = prefs.edit()
        PadAction.entries.filter { it != action && keyFor(it) == keyCode }.forEach {
            editor.putInt(it.name, KeyEvent.KEYCODE_UNKNOWN)
        }
        editor.putInt(action.name, keyCode).apply()
    }

    fun clear(action: PadAction) = prefs.edit().putInt(action.name, KeyEvent.KEYCODE_UNKNOWN).apply()

    fun reset() = prefs.edit().clear().apply()

    /** Lookup table for the emulator: every key that maps to something, plus keyboard extras. */
    fun table(): Map<Int, PadAction> {
        val map = HashMap<Int, PadAction>(KEYBOARD_EXTRAS)
        PadAction.entries.forEach { action ->
            val key = keyFor(action)
            if (key != KeyEvent.KEYCODE_UNKNOWN) map[key] = action
        }
        return map
    }

    companion object {
        val DEFAULTS = mapOf(
            PadAction.UP to KeyEvent.KEYCODE_DPAD_UP,
            PadAction.DOWN to KeyEvent.KEYCODE_DPAD_DOWN,
            PadAction.LEFT to KeyEvent.KEYCODE_DPAD_LEFT,
            PadAction.RIGHT to KeyEvent.KEYCODE_DPAD_RIGHT,
            PadAction.A to KeyEvent.KEYCODE_BUTTON_A,
            PadAction.B to KeyEvent.KEYCODE_BUTTON_B,
            PadAction.X to KeyEvent.KEYCODE_BUTTON_X,
            PadAction.Y to KeyEvent.KEYCODE_BUTTON_Y,
            PadAction.L to KeyEvent.KEYCODE_BUTTON_L1,
            PadAction.R to KeyEvent.KEYCODE_BUTTON_R1,
            PadAction.START to KeyEvent.KEYCODE_BUTTON_START,
            PadAction.SELECT to KeyEvent.KEYCODE_BUTTON_SELECT,
            PadAction.MENU to KeyEvent.KEYCODE_BUTTON_MODE,
            PadAction.FAST_FORWARD to KeyEvent.KEYCODE_BUTTON_THUMBR,
        )

        /** Keyboard fallbacks that always work (unless the key is remapped to something else). */
        private val KEYBOARD_EXTRAS = mapOf(
            KeyEvent.KEYCODE_W to PadAction.UP,
            KeyEvent.KEYCODE_S to PadAction.DOWN,
            KeyEvent.KEYCODE_A to PadAction.LEFT,
            KeyEvent.KEYCODE_D to PadAction.RIGHT,
            KeyEvent.KEYCODE_X to PadAction.A,
            KeyEvent.KEYCODE_Z to PadAction.B,
            KeyEvent.KEYCODE_Q to PadAction.L,
            KeyEvent.KEYCODE_E to PadAction.R,
            KeyEvent.KEYCODE_ENTER to PadAction.START,
            KeyEvent.KEYCODE_SPACE to PadAction.SELECT,
            KeyEvent.KEYCODE_BUTTON_L2 to PadAction.L,
            KeyEvent.KEYCODE_BUTTON_R2 to PadAction.R,
            KeyEvent.KEYCODE_F1 to PadAction.QUICK_SAVE,
            KeyEvent.KEYCODE_F4 to PadAction.QUICK_LOAD,
        )

        fun keyName(keyCode: Int): String =
            if (keyCode == KeyEvent.KEYCODE_UNKNOWN) "없음"
            else KeyEvent.keyCodeToString(keyCode).removePrefix("KEYCODE_").replace('_', ' ')
    }
}
