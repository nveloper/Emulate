package com.emulate.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.CornerPathEffect
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Build
import android.view.MotionEvent
import android.view.View
import androidx.core.content.res.ResourcesCompat
import com.google.android.material.color.MaterialColors
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Multi-touch on-screen gamepad. Touches that start on a control press it (face buttons can be
 * slid between); touches on [gameRect] are forwarded as a pointer when [touchScreenEnabled] is
 * set (the NDS touch screen).
 *
 * The built-in layout is zone based so controls never overlap: in portrait three rows below the
 * game (shoulders and menu, d-pad and face buttons, Select/Start), in landscape a column on each
 * side. Sizes are in dp and shrink only when the space can't fit them. [portraitControlHeight] and
 * [landscapeColumnWidth] tell the activity how much room to leave next to the game.
 *
 * [placements] move, resize or hide whole [TouchElement]s; in [editMode] touches drag elements
 * around instead of pressing them.
 */
class ControllerOverlay(context: Context) : View(context) {

    interface Listener {
        fun onButtons(mask: Int)
        fun onPointer(x: Int, y: Int, pressed: Boolean)
        fun onMenu()
        /** The fast-forward button went down or up. */
        fun onFastForwardButton(pressed: Boolean)
    }

    var listener: Listener? = null
    var platform: Platform = Platform.GBA
        set(value) {
            field = value
            layoutControls()
        }
    var touchScreenEnabled = false
    /** Size multiplier for all controls (user setting). */
    var sizeScale = 1f
        set(value) {
            field = value
            layoutControls()
        }
    var haptics = true
    private val hapticEngine = Haptics(context)
    /** Pauses button haptics (e.g. while the game rumbles the motor). */
    var hapticsSuppressed: Boolean
        get() = hapticEngine.suppressed
        set(value) { hapticEngine.suppressed = value }
    val gameRect = RectF()

    /** User placements for the current orientation (empty: built-in layout). */
    var placements: Map<TouchElement, TouchPlacement> = emptyMap()
        set(value) {
            field = value
            layoutControls()
        }

    /** While editing, touches select and drag elements; nothing is pressed. */
    var editMode = false
        set(value) {
            releaseAll()
            field = value
            selected = null
            dragPointerId = -1
            onSelectionChanged?.invoke(null)
            invalidate()
        }
    var selected: TouchElement? = null
        private set
    var onSelectionChanged: ((TouchElement?) -> Unit)? = null

    /**
     * The controls sit on top of the game picture (it fills the width/screen and they can't fit
     * beside it). Adds a toggle that hides all other controls, e.g. to use the whole NDS touch
     * screen.
     */
    var overlapping = false
        set(value) {
            if (field == value) return
            field = value
            if (!value) controlsHidden = false
            layoutControls()
        }

    /** Everything but the toggle is hidden (only while [overlapping]). */
    var controlsHidden = false
        private set

    /** Shown on the fast-forward button while the game runs at another speed (null: normal). */
    private var speedLabel: String? = null
    private var ffPointerId = -1

    // ---- controls ----

    private sealed class Control {
        abstract fun hit(x: Float, y: Float): Boolean
    }

    private class Button(val mask: Int, val label: String, val round: Boolean, val element: TouchElement) : Control() {
        val rect = RectF()
        override fun hit(x: Float, y: Float): Boolean {
            // A little forgiving around the edges, more so vertically for the flat pills.
            val padX = rect.width() * 0.12f
            val padY = rect.height() * (if (round) 0.12f else 0.25f)
            return x >= rect.left - padX && x <= rect.right + padX && y >= rect.top - padY && y <= rect.bottom + padY
        }
    }

    private class DPad : Control() {
        var cx = 0f
        var cy = 0f
        var radius = 0f
        override fun hit(x: Float, y: Float) = hypot(x - cx, y - cy) <= radius * 1.2f

        fun maskAt(x: Float, y: Float): Int {
            val dx = x - cx
            val dy = y - cy
            val dist = hypot(dx, dy)
            if (dist < radius * 0.15f) return 0
            // 8-way: a direction counts when within 67.5 degrees of its axis.
            var mask = 0
            if (dx > dist * 0.383f) mask = mask or Pad.RIGHT
            if (dx < -dist * 0.383f) mask = mask or Pad.LEFT
            if (dy > dist * 0.383f) mask = mask or Pad.DOWN
            if (dy < -dist * 0.383f) mask = mask or Pad.UP
            return mask
        }
    }

    private class Action(val element: TouchElement) : Control() {
        val rect = RectF()
        override fun hit(x: Float, y: Float): Boolean {
            val pad = rect.height() * 0.2f
            return x >= rect.left - pad && x <= rect.right + pad && y >= rect.top - pad && y <= rect.bottom + pad
        }
    }

    private val dpad = DPad()
    private var buttons = listOf<Button>()
    private val menuAction = Action(TouchElement.MENU)
    private val ffAction = Action(TouchElement.FAST_FORWARD)
    private val toggleAction = Action(TouchElement.TOGGLE)
    private var present = listOf<TouchElement>()

    // pointer id -> what it is holding
    private val pointerDpad = HashMap<Int, Boolean>()
    private val pointerButton = HashMap<Int, Button?>()
    private val pointerDpadMask = HashMap<Int, Int>()
    private var screenPointerId = -1
    private var lastMask = 0

    // Editing
    private var dragPointerId = -1
    private var dragOffsetX = 0f
    private var dragOffsetY = 0f

    // ---- metrics (px) ----

    private val dp = resources.displayMetrics.density

    /** Sizes at the user's scale, before fitting into the available space. */
    private inner class Metrics(compact: Boolean = false) {
        val s = sizeScale
        val margin = 16 * dp
        // Compact: tighter spacing, used when that lets the game fill the screen's width.
        val gap = (if (compact) 8 else 12) * dp
        val bottomPortrait = (if (compact) 20 else 28) * dp  // clear of the gesture navigation area
        val bottomLandscape = 16 * dp
        val face = 66 * dp * s
        val faceGap = 10 * dp * s
        val dpad = 160 * dp * s
        val shoulderW = 96 * dp * s
        val shoulderH = 42 * dp * s
        val pillW = 80 * dp * s
        val pillH = 36 * dp * s
        val actionW = 58 * dp * s
        val actionH = 36 * dp * s
        val fourFace = platform == Platform.NDS
        val shoulders = platform == Platform.GBA || platform == Platform.NDS
        val mic = platform == Platform.NDS

        /** Width and height of the face button cluster for a given button size. */
        fun cluster(faceSize: Float, gapSize: Float): Pair<Float, Float> = if (fourFace) {
            val r = (faceSize + gapSize) / sqrt(2f)
            (2 * r + faceSize) to (2 * r + faceSize)
        } else {
            val half = (faceSize + gapSize) / 2
            (2 * half * COS_FACE + faceSize) to (2 * half * SIN_FACE + faceSize)
        }

        val topRowH get() = if (shoulders) max(shoulderH, actionH) else actionH
    }

    /** Height the controls need below the game in portrait at full size. */
    fun portraitControlHeight(): Float {
        val m = Metrics()
        val (_, clusterH) = m.cluster(m.face, m.faceGap)
        return m.gap + m.topRowH + m.gap + max(m.dpad, clusterH) + m.gap + m.pillH + m.bottomPortrait
    }

    /** Height actually used by the portrait controls on a screen [height] px tall. */
    fun portraitControlArea(height: Float): Float = min(portraitArea.takeIf { it > 0f } ?: portraitControlHeight(), height * 0.55f)

    /**
     * Height given to the portrait controls (0: their natural height). The activity lowers it
     * towards [portraitMinimumHeight] when that lets the game fill the width.
     */
    var portraitArea = 0f

    /** The least height the portrait controls can work with: compact spacing, pad at 85%. */
    fun portraitMinimumHeight(): Float {
        val m = Metrics(compact = true)
        val (_, clusterH) = m.cluster(m.face, m.faceGap)
        return m.gap + m.topRowH + m.gap + max(m.dpad, clusterH) * 0.85f + m.gap + m.pillH + m.bottomPortrait
    }

    /** Width of each side column in landscape at full size (or as much as [height] allows). */
    fun landscapeColumnWidth(height: Int): Float {
        val m = Metrics()
        val k = landscapeMainScale(m, height.toFloat())
        val (clusterW, _) = m.cluster(m.face * k, m.faceGap * k)
        val row2 = m.actionW + if (m.mic) m.gap + m.pillW else 0f
        val inner = maxOf(m.dpad * k, clusterW, m.shoulderW, row2, m.pillW)
        return m.margin + inner + m.gap
    }

    private fun landscapeMainScale(m: Metrics, h: Float): Float {
        val top = m.margin + m.topRowH + m.gap + m.actionH + m.gap
        val bottom = h - m.bottomLandscape - m.pillH - m.gap
        val (_, clusterH) = m.cluster(m.face, m.faceGap)
        return ((bottom - top) / max(m.dpad, clusterH)).coerceIn(0.5f, 1f)
    }

    // ---- paints (Material You colours from the activity's theme) ----

    private fun themeColor(attr: Int, fallback: Int) = MaterialColors.getColor(context, attr, fallback)
    private fun Int.withAlpha(alpha: Int) = (this and 0x00FFFFFF) or (alpha shl 24)

    private val surfaceColor = themeColor(com.google.android.material.R.attr.colorSurfaceContainerHighest, 0xFF36343B.toInt()).withAlpha(0xD9)
    private val onSurfaceColor = themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant, 0xFFCAC4D0.toInt())
    private val outlineColor = themeColor(com.google.android.material.R.attr.colorOutlineVariant, 0xFF49454F.toInt())
    private val faceColor = themeColor(com.google.android.material.R.attr.colorPrimaryContainer, 0xFF4F378B.toInt()).withAlpha(0xE6)
    private val onFaceColor = themeColor(com.google.android.material.R.attr.colorOnPrimaryContainer, 0xFFEADDFF.toInt())
    private val pressedColor = themeColor(androidx.appcompat.R.attr.colorPrimary, 0xFFD0BCFF.toInt())
    private val onPressedColor = themeColor(com.google.android.material.R.attr.colorOnPrimary, 0xFF381E72.toInt())
    private val activeColor = themeColor(com.google.android.material.R.attr.colorTertiaryContainer, 0xFF633B48.toInt()).withAlpha(0xE6)
    private val onActiveColor = themeColor(com.google.android.material.R.attr.colorOnTertiaryContainer, 0xFFFFD8E4.toInt())

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = outlineColor
        strokeWidth = 1 * dp
    }
    private val glyph = Paint(Paint.ANTI_ALIAS_FLAG).apply { pathEffect = CornerPathEffect(2 * dp) }
    private val glyphStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        val sans = runCatching { ResourcesCompat.getFont(context, R.font.google_sans) }.getOrNull() ?: Typeface.DEFAULT
        typeface = if (Build.VERSION.SDK_INT >= 28) Typeface.create(sans, 600, false) else Typeface.create(sans, Typeface.BOLD)
    }
    private val selection = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = pressedColor
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * dp
        pathEffect = DashPathEffect(floatArrayOf(8 * dp, 5 * dp), 0f)
    }
    private val guide = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = pressedColor.withAlpha(0x80)
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * dp
    }
    private val path = Path()
    private val scratch = Path()

    // ---- layout ----

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = layoutControls()

    /** Lays out controls; call after [gameRect] changes. */
    fun layoutControls() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w == 0f || h == 0f) return
        val m = Metrics(compact = h > w && portraitArea > 0f && portraitArea < portraitControlHeight() - 0.5f)
        val list = ArrayList<Button>()
        if (h > w) layoutPortrait(m, w, h, list) else layoutLandscape(m, w, h, list)
        buttons = list

        present = TouchElement.entries.filter { element ->
            when (element) {
                TouchElement.L, TouchElement.R -> m.shoulders
                TouchElement.MIC -> m.mic
                TouchElement.TOGGLE -> overlapping
                else -> true
            }
        }
        // User placements: scale each element about its default centre, then move it.
        for (element in present) {
            val p = placements[element] ?: continue
            val bounds = bounds(element)
            transform(element, bounds.centerX(), bounds.centerY(), p.scale,
                (p.x * w).coerceIn(0f, w), (p.y * h).coerceIn(0f, h))
        }
        invalidate()
    }

    private fun RectF.centeredAt(cx: Float, cy: Float, w: Float, h: Float) = apply { set(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2) }

    private fun pill(list: MutableList<Button>, mask: Int, label: String, element: TouchElement, cx: Float, cy: Float, w: Float, h: Float) {
        list += Button(mask, label, false, element).apply { rect.centeredAt(cx, cy, w, h) }
    }

    private fun faceButtons(list: MutableList<Button>, m: Metrics, cx: Float, cy: Float, k: Float) {
        val face = m.face * k
        val gap = m.faceGap * k
        fun circle(mask: Int, label: String, x: Float, y: Float) {
            list += Button(mask, label, true, TouchElement.FACE).apply { rect.centeredAt(x, y, face, face) }
        }
        if (m.fourFace) {
            val r = (face + gap) / sqrt(2f)
            circle(Pad.X, "X", cx, cy - r)
            circle(Pad.A, "A", cx + r, cy)
            circle(Pad.B, "B", cx, cy + r)
            circle(Pad.Y, "Y", cx - r, cy)
        } else {
            val half = (face + gap) / 2
            circle(Pad.A, "A", cx + half * COS_FACE, cy - half * SIN_FACE)
            circle(Pad.B, "B", cx - half * COS_FACE, cy + half * SIN_FACE)
        }
    }

    private fun layoutPortrait(m: Metrics, w: Float, h: Float, list: MutableList<Button>) {
        // A compact block at the bottom: the controls keep their natural height instead of
        // stretching into whatever the game leaves free.
        val top = h - portraitControlArea(h)

        // Row 1: L ... [menu][fast-forward] ... R (narrowed if the screen is too slim).
        val rowY = top + m.gap + m.topRowH / 2
        val actions = if (overlapping) 3 else 2
        val row1Need = 2 * m.margin + actions * m.actionW + (actions - 1) * m.gap + if (m.shoulders) 2 * (m.shoulderW + m.gap) else 0f
        val k1 = min(1f, w / row1Need)
        val actionW = m.actionW * k1
        // [menu][fast-forward] or [menu][fast-forward][toggle], centred.
        val firstX = w / 2 - (actions - 1) * (actionW + m.gap) / 2
        menuAction.rect.centeredAt(firstX, rowY, actionW, m.actionH)
        ffAction.rect.centeredAt(firstX + actionW + m.gap, rowY, actionW, m.actionH)
        toggleAction.rect.centeredAt(firstX + 2 * (actionW + m.gap), rowY, actionW, m.actionH)
        if (m.shoulders) {
            val sw = m.shoulderW * k1
            pill(list, Pad.L, "L", TouchElement.L, m.margin + sw / 2, rowY, sw, m.shoulderH)
            pill(list, Pad.R, "R", TouchElement.R, w - m.margin - sw / 2, rowY, sw, m.shoulderH)
        }

        // Row 3: [SELECT][START] centred, MIC at the right edge.
        val bottomY = h - m.bottomPortrait - m.pillH / 2
        val halfNeed = m.gap / 2 + m.pillW + m.margin + if (m.mic) m.gap + m.pillW else 0f
        val pw = m.pillW * min(1f, (w / 2) / halfNeed)
        pill(list, Pad.SELECT, "SELECT", TouchElement.SELECT, w / 2 - m.gap / 2 - pw / 2, bottomY, pw, m.pillH)
        pill(list, Pad.START, "START", TouchElement.START, w / 2 + m.gap / 2 + pw / 2, bottomY, pw, m.pillH)
        if (m.mic) pill(list, Pad.L3, "MIC", TouchElement.MIC, w - m.margin - pw / 2, bottomY, pw, m.pillH)

        // Row 2: d-pad and face buttons, as large as the space between the rows allows.
        val mainTop = rowY + m.topRowH / 2 + m.gap
        val mainBottom = bottomY - m.pillH / 2 - m.gap
        val (clusterW, clusterH) = m.cluster(m.face, m.faceGap)
        val k = minOf(1f, (mainBottom - mainTop) / max(m.dpad, clusterH), (w - 2 * m.margin - m.gap) / (m.dpad + clusterW))
            .coerceAtLeast(0.4f)
        val cy = (mainTop + mainBottom) / 2
        val dpadR = m.dpad * k / 2
        val (cw, _) = m.cluster(m.face * k, m.faceGap * k)
        // Pull both towards the thumbs' natural resting spots on wide screens.
        dpad.cx = max(m.margin + dpadR, min(w * 0.26f, w / 2 - m.gap / 2 - dpadR))
        dpad.cy = cy
        dpad.radius = dpadR
        faceButtons(list, m, w - max(m.margin + cw / 2, min(w * 0.26f, w / 2 - m.gap / 2 - cw / 2)), cy, k)
    }

    private fun layoutLandscape(m: Metrics, w: Float, h: Float, list: MutableList<Button>) {
        val k = landscapeMainScale(m, h)
        val colW = landscapeColumnWidth(h.toInt())
        val inner = colW - m.margin - m.gap
        val leftX = m.margin + inner / 2
        val rightX = w - leftX

        // Top: shoulders.
        val rowY = m.margin + m.topRowH / 2
        if (m.shoulders) {
            pill(list, Pad.L, "L", TouchElement.L, leftX, rowY, m.shoulderW, m.shoulderH)
            pill(list, Pad.R, "R", TouchElement.R, rightX, rowY, m.shoulderW, m.shoulderH)
        }
        // Second row: [menu][MIC] on the left, [fast-forward] on the right.
        val row2Y = rowY + m.topRowH / 2 + m.gap + m.actionH / 2
        val row2W = m.actionW + if (m.mic) m.gap + m.pillW else 0f
        menuAction.rect.centeredAt(leftX - row2W / 2 + m.actionW / 2, row2Y, m.actionW, m.actionH)
        if (m.mic) pill(list, Pad.L3, "MIC", TouchElement.MIC, leftX + row2W / 2 - m.pillW / 2, row2Y, m.pillW, m.actionH)
        if (overlapping) {
            ffAction.rect.centeredAt(rightX - (m.actionW + m.gap) / 2, row2Y, m.actionW, m.actionH)
            toggleAction.rect.centeredAt(rightX + (m.actionW + m.gap) / 2, row2Y, m.actionW, m.actionH)
        } else {
            ffAction.rect.centeredAt(rightX, row2Y, m.actionW, m.actionH)
        }

        // Bottom: Select / Start.
        val bottomY = h - m.bottomLandscape - m.pillH / 2
        pill(list, Pad.SELECT, "SELECT", TouchElement.SELECT, leftX, bottomY, m.pillW, m.pillH)
        pill(list, Pad.START, "START", TouchElement.START, rightX, bottomY, m.pillW, m.pillH)

        // Middle: d-pad and face buttons.
        val cy = (row2Y + m.actionH / 2 + m.gap + bottomY - m.pillH / 2 - m.gap) / 2
        dpad.cx = leftX
        dpad.cy = cy
        dpad.radius = m.dpad * k / 2
        faceButtons(list, m, rightX, cy, k)
    }

    private fun transform(element: TouchElement, cx: Float, cy: Float, s: Float, tx: Float, ty: Float) {
        fun RectF.map() = set(tx + (left - cx) * s, ty + (top - cy) * s, tx + (right - cx) * s, ty + (bottom - cy) * s)
        when (element) {
            TouchElement.DPAD -> {
                dpad.cx = tx
                dpad.cy = ty
                dpad.radius *= s
            }
            TouchElement.MENU -> menuAction.rect.map()
            TouchElement.FAST_FORWARD -> ffAction.rect.map()
            TouchElement.TOGGLE -> toggleAction.rect.map()
            else -> buttons.filter { it.element == element }.forEach { it.rect.map() }
        }
    }

    private fun bounds(element: TouchElement): RectF = when (element) {
        TouchElement.DPAD -> RectF(dpad.cx - dpad.radius, dpad.cy - dpad.radius, dpad.cx + dpad.radius, dpad.cy + dpad.radius)
        TouchElement.MENU -> RectF(menuAction.rect)
        TouchElement.FAST_FORWARD -> RectF(ffAction.rect)
        TouchElement.TOGGLE -> RectF(toggleAction.rect)
        else -> RectF().also { r -> buttons.filter { it.element == element }.forEach { r.union(it.rect) } }
    }

    private fun visible(element: TouchElement) = element in present && placements[element]?.hidden != true &&
        (!controlsHidden || editMode || element == TouchElement.TOGGLE)

    // ---- drawing ----

    override fun onDraw(canvas: Canvas) {
        val held = lastMask

        fun withVisibility(element: TouchElement, draw: () -> Unit) {
            if (visible(element)) {
                draw()
            } else if (editMode && element in present) {
                // Hidden elements stay visible (faint) while editing so they can be shown again.
                val saved = canvas.saveLayerAlpha(null, 0x50)
                draw()
                canvas.restoreToCount(saved)
            }
        }

        withVisibility(TouchElement.DPAD) { drawDpad(canvas, held) }
        for (b in buttons) withVisibility(b.element) { drawButton(canvas, b, held and b.mask != 0) }
        withVisibility(TouchElement.MENU) { drawAction(canvas, menuAction, active = false) }
        withVisibility(TouchElement.FAST_FORWARD) { drawAction(canvas, ffAction, active = speedLabel != null) }
        withVisibility(TouchElement.TOGGLE) { drawAction(canvas, toggleAction, active = controlsHidden) }

        if (editMode) {
            selected?.let { element ->
                val r = bounds(element)
                val pad = 6 * dp
                r.inset(-pad, -pad)
                canvas.drawRoundRect(r, 16 * dp, 16 * dp, selection)
                // Centre guide while the element sits on the vertical middle line (snapped).
                if (dragPointerId != -1 && abs(r.centerX() - width / 2f) < 1f) {
                    canvas.drawLine(width / 2f, 0f, width / 2f, height.toFloat(), guide)
                }
            }
        }
    }

    private fun drawLabel(canvas: Canvas, label: String, cx: Float, cy: Float, size: Float, color: Int, spacing: Float = 0f) {
        text.textSize = size
        text.letterSpacing = spacing
        text.color = color
        canvas.drawText(label, cx, cy - (text.descent() + text.ascent()) / 2, text)
    }

    /** Draws with a slight shrink while pressed, like a key going down. */
    private inline fun pressable(canvas: Canvas, pressed: Boolean, cx: Float, cy: Float, draw: () -> Unit) {
        if (!pressed) return draw()
        canvas.save()
        canvas.scale(0.94f, 0.94f, cx, cy)
        draw()
        canvas.restore()
    }

    private fun drawDpad(canvas: Canvas, held: Int) {
        val r = dpad.radius
        val arm = r * 0.36f
        val cx = dpad.cx
        val cy = dpad.cy
        val corner = arm * 0.55f
        // One cross-shaped outline with generously rounded ends.
        path.reset()
        path.addRoundRect(cx - r, cy - arm, cx + r, cy + arm, corner, corner, Path.Direction.CW)
        scratch.reset()
        scratch.addRoundRect(cx - arm, cy - r, cx + arm, cy + r, corner, corner, Path.Direction.CW)
        path.op(scratch, Path.Op.UNION)
        fill.color = surfaceColor
        canvas.drawPath(path, fill)

        // Held directions fill their arm with the accent colour (diagonals light two arms).
        val arms = listOf(Pad.UP to (0f to -1f), Pad.DOWN to (0f to 1f), Pad.LEFT to (-1f to 0f), Pad.RIGHT to (1f to 0f))
        if (held != 0) {
            canvas.save()
            canvas.clipPath(path)
            fill.color = pressedColor
            for ((bit, dir) in arms) {
                if (held and bit == 0) continue
                val (dx, dy) = dir
                val near = arm * 0.25f
                canvas.drawRect(
                    if (dx < 0) cx - r else if (dx > 0) cx + near else cx - arm,
                    if (dy < 0) cy - r else if (dy > 0) cy + near else cy - arm,
                    if (dx > 0) cx + r else if (dx < 0) cx - near else cx + arm,
                    if (dy > 0) cy + r else if (dy < 0) cy - near else cy + arm,
                    fill,
                )
            }
            canvas.restore()
        }
        canvas.drawPath(path, outline)

        // Rounded arrow on each arm.
        val tip = r * 0.78f
        val size = arm * 0.5f
        for ((bit, dir) in arms) {
            val (dx, dy) = dir
            val tx = cx + dx * tip
            val ty = cy + dy * tip
            val bx = tx - dx * size
            val by = ty - dy * size
            scratch.reset()
            scratch.moveTo(tx, ty)
            scratch.lineTo(bx + dy * size * 0.9f, by - dx * size * 0.9f)
            scratch.lineTo(bx - dy * size * 0.9f, by + dx * size * 0.9f)
            scratch.close()
            glyph.color = if (held and bit != 0) onPressedColor else onSurfaceColor
            canvas.drawPath(scratch, glyph)
        }
    }

    private fun drawButton(canvas: Canvas, b: Button, pressed: Boolean) {
        val rect = b.rect
        pressable(canvas, pressed, rect.centerX(), rect.centerY()) {
            val radius = if (b.round) rect.width() / 2 else rect.height() / 2
            fill.color = when {
                pressed -> pressedColor
                b.round -> faceColor
                else -> surfaceColor
            }
            canvas.drawRoundRect(rect, radius, radius, fill)
            if (!b.round && !pressed) canvas.drawRoundRect(rect, radius, radius, outline)
            val color = when {
                pressed -> onPressedColor
                b.round -> onFaceColor
                else -> onSurfaceColor
            }
            when {
                b.round -> drawLabel(canvas, b.label, rect.centerX(), rect.centerY(), rect.width() * 0.4f, color)
                b.label.length <= 2 -> drawLabel(canvas, b.label, rect.centerX(), rect.centerY(), rect.height() * 0.46f, color)
                else -> drawLabel(canvas, b.label, rect.centerX(), rect.centerY(), rect.height() * 0.36f, color, spacing = 0.05f)
            }
        }
    }

    private fun drawAction(canvas: Canvas, action: Action, active: Boolean) {
        val rect = action.rect
        val h = rect.height()
        fill.color = if (active) activeColor else surfaceColor
        canvas.drawRoundRect(rect, h / 2, h / 2, fill)
        if (!active) canvas.drawRoundRect(rect, h / 2, h / 2, outline)
        val cx = rect.centerX()
        val cy = rect.centerY()
        if (active && action === ffAction && speedLabel != null) {
            drawLabel(canvas, speedLabel!!, cx, cy, h * 0.42f, onActiveColor)
            return
        }
        val color = if (active) onActiveColor else onSurfaceColor
        if (action === toggleAction) {
            // "visibility" / "visibility_off": an eye, struck through while the controls are hidden.
            glyphStroke.color = color
            glyphStroke.strokeWidth = h * 0.07f
            val ew = h * 0.27f
            val eh = h * 0.17f
            scratch.reset()
            scratch.moveTo(cx - ew, cy)
            scratch.quadTo(cx, cy - eh * 2, cx + ew, cy)
            scratch.quadTo(cx, cy + eh * 2, cx - ew, cy)
            scratch.close()
            canvas.drawPath(scratch, glyphStroke)
            glyph.color = color
            canvas.drawCircle(cx, cy, h * 0.08f, glyph)
            if (controlsHidden) canvas.drawLine(cx - ew * 0.9f, cy + ew * 0.9f, cx + ew * 0.9f, cy - ew * 0.9f, glyphStroke)
        } else if (action === menuAction) {
            // "menu": three rounded lines.
            glyphStroke.color = color
            glyphStroke.strokeWidth = h * 0.075f
            val half = h * 0.2f
            for (i in -1..1) canvas.drawLine(cx - half, cy + i * h * 0.15f, cx + half, cy + i * h * 0.15f, glyphStroke)
        } else {
            // "fast_forward": two rounded triangles.
            glyph.color = color
            val tw = h * 0.25f
            val th = h * 0.21f
            for (offset in listOf(-tw * 0.45f, tw * 0.55f)) {
                scratch.reset()
                scratch.moveTo(cx + offset - tw / 2, cy - th)
                scratch.lineTo(cx + offset + tw / 2, cy)
                scratch.lineTo(cx + offset - tw / 2, cy + th)
                scratch.close()
                canvas.drawPath(scratch, glyph)
            }
        }
    }

    fun showSpeed(label: String?) {
        speedLabel = label
        invalidate()
    }

    // ---- editing ----

    /** The element's placement (its current position if it has no user placement yet). */
    private fun currentPlacement(element: TouchElement): TouchPlacement {
        placements[element]?.let { return it }
        val b = bounds(element)
        return TouchPlacement(b.centerX() / width, b.centerY() / height)
    }

    val selectedPlacement: TouchPlacement? get() = selected?.let { currentPlacement(it) }

    /** Changes the selected element's size by [delta] (0.5x .. 2x). */
    fun resizeSelected(delta: Float) {
        val element = selected ?: return
        val p = currentPlacement(element)
        val scale = ((p.scale + delta) * 10f).roundToInt() / 10f
        placements = placements + (element to p.copy(scale = scale.coerceIn(0.5f, 2f)))
        onSelectionChanged?.invoke(element)
    }

    fun toggleSelectedHidden() {
        val element = selected ?: return
        val p = currentPlacement(element)
        placements = placements + (element to p.copy(hidden = !p.hidden))
        onSelectionChanged?.invoke(element)
    }

    private fun elementAt(x: Float, y: Float): TouchElement? {
        // Small controls first so they can be picked up even when they sit on a bigger one.
        val order = listOf(
            TouchElement.TOGGLE, TouchElement.MENU, TouchElement.FAST_FORWARD, TouchElement.L, TouchElement.R, TouchElement.SELECT,
            TouchElement.START, TouchElement.MIC, TouchElement.FACE, TouchElement.DPAD,
        )
        val pad = 8 * dp
        return order.filter { it in present }.firstOrNull { element ->
            bounds(element).apply { inset(-pad, -pad) }.contains(x, y)
        }
    }

    private fun editTouch(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = event.x
                val y = event.y
                selected = elementAt(x, y)
                selected?.let { element ->
                    val b = bounds(element)
                    dragOffsetX = x - b.centerX()
                    dragOffsetY = y - b.centerY()
                    dragPointerId = event.getPointerId(0)
                    if (haptics) hapticEngine.press(this)
                }
                onSelectionChanged?.invoke(selected)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val element = selected ?: return
                val i = event.findPointerIndex(dragPointerId)
                if (i < 0) return
                val w = width.toFloat()
                val h = height.toFloat()
                var cx = (event.getX(i) - dragOffsetX).coerceIn(0f, w)
                val cy = (event.getY(i) - dragOffsetY).coerceIn(0f, h)
                if (abs(cx - w / 2) < 10 * dp) cx = w / 2  // snap to the middle
                placements = placements + (element to currentPlacement(element).copy(x = cx / w, y = cy / h))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragPointerId = -1
                invalidate()
            }
        }
    }

    // ---- playing ----

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (editMode) {
            editTouch(event)
            return true
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                pointerDown(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) pointerMove(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> pointerUp(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        updateMask()
        return true
    }

    private fun releaseAll() {
        pointerDpad.clear()
        pointerButton.clear()
        pointerDpadMask.clear()
        if (ffPointerId != -1) {
            ffPointerId = -1
            listener?.onFastForwardButton(false)
        }
        if (screenPointerId != -1) {
            screenPointerId = -1
            listener?.onPointer(0, 0, false)
        }
        updateMask()
    }

    private fun hitButton(x: Float, y: Float) = buttons.firstOrNull { visible(it.element) && it.hit(x, y) }

    private fun pointerDown(id: Int, x: Float, y: Float) {
        if (visible(TouchElement.TOGGLE) && toggleAction.hit(x, y)) {
            if (haptics) hapticEngine.press(this)
            val keep = screenPointerId
            pointerDpad.clear()
            pointerDpadMask.clear()
            pointerButton.clear()
            screenPointerId = keep
            controlsHidden = !controlsHidden
            invalidate()
            return
        }
        if (visible(TouchElement.MENU) && menuAction.hit(x, y)) {
            if (haptics) hapticEngine.press(this)
            listener?.onMenu()
            return
        }
        if (visible(TouchElement.FAST_FORWARD) && ffAction.hit(x, y)) {
            if (haptics) hapticEngine.press(this)
            if (ffPointerId == -1) {
                ffPointerId = id
                listener?.onFastForwardButton(true)
            }
            return
        }
        // Buttons before the d-pad: a moved button may overlap the d-pad's touch area.
        val button = hitButton(x, y)
        if (button != null) {
            pointerButton[id] = button
            return
        }
        if (visible(TouchElement.DPAD) && dpad.hit(x, y)) {
            pointerDpad[id] = true
            pointerDpadMask[id] = dpad.maskAt(x, y)
            return
        }
        if (touchScreenEnabled && screenPointerId == -1 && gameRect.contains(x, y)) {
            screenPointerId = id
            sendPointer(x, y, true)
        }
    }

    private fun pointerMove(id: Int, x: Float, y: Float) {
        when {
            pointerDpad[id] == true -> pointerDpadMask[id] = dpad.maskAt(x, y)
            pointerButton.containsKey(id) -> {
                // Slide between buttons (e.g. A -> B) without lifting the finger.
                pointerButton[id] = hitButton(x, y)
            }
            id == screenPointerId -> sendPointer(x, y, true)
        }
    }

    private fun pointerUp(id: Int) {
        if (id == ffPointerId) {
            ffPointerId = -1
            listener?.onFastForwardButton(false)
        }
        pointerDpad.remove(id)
        pointerDpadMask.remove(id)
        pointerButton.remove(id)
        if (id == screenPointerId) {
            screenPointerId = -1
            listener?.onPointer(0, 0, false)
        }
    }

    private fun sendPointer(x: Float, y: Float, pressed: Boolean) {
        if (gameRect.isEmpty) return
        val nx = ((x - gameRect.left) / gameRect.width()).coerceIn(0f, 1f) * 2f - 1f
        val ny = ((y - gameRect.top) / gameRect.height()).coerceIn(0f, 1f) * 2f - 1f
        listener?.onPointer((nx * 0x7FFF).toInt(), (ny * 0x7FFF).toInt(), pressed)
    }

    private fun updateMask() {
        var mask = 0
        pointerDpadMask.values.forEach { mask = mask or it }
        pointerButton.values.forEach { b -> if (b != null) mask = mask or b.mask }
        if (mask != lastMask) {
            // Click when something goes down, a lighter tick when something comes up.
            if (haptics) {
                if (mask and lastMask.inv() != 0) hapticEngine.press(this)
                else if (lastMask and mask.inv() != 0) hapticEngine.release(this)
            }
            lastMask = mask
            listener?.onButtons(mask)
            invalidate()
        }
    }

    private companion object {
        // A/B sit on a 25 degree diagonal, like a Game Boy.
        val COS_FACE = cos(Math.toRadians(25.0)).toFloat()
        val SIN_FACE = sin(Math.toRadians(25.0)).toFloat()
    }
}
