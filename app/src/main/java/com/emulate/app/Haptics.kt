package com.emulate.app

import android.content.Context
import android.os.Build
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.HapticFeedbackConstants
import android.view.View

/**
 * Button haptics: a crisp "click" when a control goes down and a lighter "tick" when it comes up.
 *
 * Phones with a linear (LRA) motor get the motor's own short primitives (Android 11+), which feel
 * like a physical key; others get the predefined click/tick effects (Android 10+), and older
 * phones the view's standard keyboard haptic.
 */
class Haptics(private val context: Context) {
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager).defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
    }?.takeIf { it.hasVibrator() }

    private val pressEffect: VibrationEffect?
    private val releaseEffect: VibrationEffect?

    private val systemTouchFeedback: Boolean
        get() = android.provider.Settings.System.getInt(
            context.contentResolver, android.provider.Settings.System.HAPTIC_FEEDBACK_ENABLED, 1) != 0

    /** While the game rumbles, button clicks would cut the rumble short: skip them. */
    @Volatile
    var suppressed = false

    init {
        val v = vibrator
        when {
            v == null -> {
                pressEffect = null
                releaseEffect = null
            }
            Build.VERSION.SDK_INT >= 30 &&
                v.areAllPrimitivesSupported(VibrationEffect.Composition.PRIMITIVE_CLICK, VibrationEffect.Composition.PRIMITIVE_TICK) -> {
                pressEffect = VibrationEffect.startComposition()
                    .addPrimitive(VibrationEffect.Composition.PRIMITIVE_CLICK, 0.9f).compose()
                releaseEffect = VibrationEffect.startComposition()
                    .addPrimitive(VibrationEffect.Composition.PRIMITIVE_TICK, 0.6f).compose()
            }
            Build.VERSION.SDK_INT >= 29 -> {
                pressEffect = VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK)
                releaseEffect = VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK)
            }
            else -> {
                pressEffect = null
                releaseEffect = null
            }
        }
    }

    fun press(view: View) = play(view, pressEffect, HapticFeedbackConstants.KEYBOARD_TAP)

    fun release(view: View) {
        // Without a crisp effect a release buzz would feel mushy; only press then.
        if (releaseEffect != null) play(view, releaseEffect, -1)
    }

    private fun play(view: View, effect: VibrationEffect?, fallback: Int) {
        if (suppressed) return
        val v = vibrator
        if (effect != null && v != null) {
            if (Build.VERSION.SDK_INT >= 33) {
                // Touch usage follows the system's touch-feedback intensity. If the user turned
                // system touch feedback off it would be muted entirely, though they switched
                // button haptics on in this app - play it as media vibration then.
                v.vibrate(effect, VibrationAttributes.createForUsage(
                    if (systemTouchFeedback) VibrationAttributes.USAGE_TOUCH else VibrationAttributes.USAGE_MEDIA))
            } else {
                v.vibrate(effect)
            }
        } else if (fallback >= 0) {
            view.performHapticFeedback(fallback)
        }
    }
}
