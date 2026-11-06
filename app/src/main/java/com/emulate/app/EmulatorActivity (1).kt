package com.emulate.app

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.net.wifi.WifiManager
import android.text.InputType
import android.widget.TextView
import android.widget.Toast
import androidx.activity.addCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.CircularProgressIndicator
import com.google.android.material.shape.MaterialShapeDrawable
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.text.DateFormat
import java.util.Date
import kotlin.concurrent.thread
import kotlin.math.max

class EmulatorActivity : AppCompatActivity(), SurfaceHolder.Callback, NativeBridge.Listener, ControllerOverlay.Listener {

    companion object {
        private const val EXTRA_GAME = "game"

        fun intent(context: Context, game: Game) =
            Intent(context, EmulatorActivity::class.java).putExtra(EXTRA_GAME, game.id)
    }

    private lateinit var library: GameLibrary
    private lateinit var settings: Settings
    private val vibrator by lazy {
        if (Build.VERSION.SDK_INT >= 31) {
            (getSystemService(VIBRATOR_MANAGER_SERVICE) as android.os.VibratorManager).defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(VIBRATOR_SERVICE) as android.os.Vibrator
        }
    }
    private lateinit var game: Game
    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: ControllerOverlay
    private lateinit var progress: CircularProgressIndicator

    private var videoWidth = 240
    private var videoHeight = 160
    private var started = false
    private var menuOpen = false
    private var resumed = false
    private var touchButtons = 0
    private var padButtons = 0
    private var padAxisButtons = 0
    private var keyTable: Map<Int, PadAction> = emptyMap()
    private var pulseButtons = 0  // briefly held by menu actions (e.g. the NDS lid)
    private var speed = 1f

    // Touch layout editing
    private lateinit var touchLayouts: TouchLayoutStore
    private var layoutLandscape = false      // orientation the overlay's placements belong to
    private var editingLayout = false
    private var placementsBeforeEdit: Map<TouchElement, TouchPlacement> = emptyMap()
    private lateinit var editBar: android.widget.LinearLayout
    private lateinit var editLabel: TextView
    private lateinit var editSmaller: MaterialButton
    private lateinit var editLarger: MaterialButton
    private lateinit var editHide: MaterialButton

    private lateinit var statsLabel: TextView
    private val statsUpdater = object : Runnable {
        override fun run() {
            updateStats()
            root.postDelayed(this, 1000)
        }
    }

    private lateinit var netplayLabel: TextView
    private var netplayState = Netplay.State.IDLE
    private var netplayPlayers = 0
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        library = GameLibrary(this)
        settings = Settings(this)
        touchLayouts = TouchLayoutStore(this)
        game = intent.getStringExtra(EXTRA_GAME)?.let { library.find(it) } ?: run {
            finish()
            return
        }
        when (game.platform) {
            Platform.GB, Platform.GBC -> { videoWidth = 160; videoHeight = 144 }
            Platform.GBA -> { videoWidth = 240; videoHeight = 160 }
            Platform.NDS -> { videoWidth = 256; videoHeight = 384 }
        }

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        overlay = ControllerOverlay(this).apply {
            platform = game.platform
            touchScreenEnabled = game.platform == Platform.NDS
            listener = this@EmulatorActivity
            alpha = settings.controlsOpacity
            haptics = settings.haptics
            sizeScale = settings.controlsScale
        }
        layoutLandscape = isLandscape
        overlay.placements = touchLayouts.load(game.platform, layoutLandscape)
        overlay.onSelectionChanged = { updateEditBar() }
        progress = CircularProgressIndicator(this).apply { isIndeterminate = true }
        // Full screen: the game picture is drawn into its rectangle, ambient light around it.
        root.addView(surfaceView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(overlay, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(progress, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        // Status chips at the top centre, clear of the shoulder buttons.
        netplayLabel = statusChip()
        statsLabel = statusChip().apply { fontFeatureSettings = "tnum" }  // steady digit widths
        root.addView(android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            addView(netplayLabel)
            addView(statsLabel, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { topMargin = dp(6) })
        }, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL)
            .apply { topMargin = dp(12) })
        buildEditBar()
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> layoutGame() }
        setContentView(root)

        NativeBridge.listener = this
        NativeBridge.setVideoFilter(settings.videoFilterIndex)
        NativeBridge.setAmbient(settings.ambient)
        NativeBridge.setSpeed(1f)
        onBackPressedDispatcher.addCallback(this) { onBackRequested() }
        startEmulation()
    }

    private fun startEmulation() {
        val slot2 = if (game.platform == Platform.NDS) library.slot2Game(game) else null
        val options = (game.platform.coreOptions + settings.coreOptions(game.platform)).toMutableMap()
        if (game.platform == Platform.NDS) {
            options["melonds_mac_address_mode"] = Netplay.macAddress(this)
            options["melonds_screen_layout1"] = settings.ndsLayout(isLandscape)
        }
        thread(name = "emu-loader") {
            val error = NativeBridge.start(
                corePath = game.platform.corePath,
                romPath = game.file.absolutePath,
                savePath = library.savePath(game).absolutePath,
                rtcPath = library.rtcPath(game).absolutePath,
                systemDir = library.systemDir.absolutePath,
                saveDir = library.saveDir.absolutePath,
                slot2RomPath = slot2?.file?.absolutePath,
                slot2SavePath = slot2?.let { library.savePath(it).absolutePath },
                optionKeys = options.keys.toTypedArray(),
                optionValues = options.values.toTypedArray(),
                language = NativeBridge.retroLanguage(),
            )
            runOnUiThread {
                progress.visibility = android.view.View.GONE
                if (error != null) {
                    if (isFinishing || isDestroyed) return@runOnUiThread
                    MaterialAlertDialogBuilder(this)
                        .setTitle("실행할 수 없습니다")
                        .setMessage(error)
                        .setPositiveButton("확인") { _, _ -> finish() }
                        .setOnCancelListener { finish() }
                        .show()
                } else {
                    started = true
                    NativeBridge.setSpeedAudio(settings.fastForwardAudio)
                    NativeBridge.setVsyncPacing(settings.vsync)
                    NativeBridge.setAudioEnabled(settings.audio)
                    applyCheats()
                    if (slot2 != null) toast("GBA 슬롯: ${slot2.title}")
                    NativeBridge.setPaused(!resumed || menuOpen)
                }
            }
        }
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun themeColor(attr: Int) = MaterialColors.getColor(this, attr, Color.GRAY)

    /** Small Material 3 surface "chip" for status text over the game. */
    private fun statusChip() = TextView(this).apply {
        setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_LabelMedium)
        typeface = androidx.core.content.res.ResourcesCompat.getFont(context, R.font.google_sans)
        setTextColor(themeColor(com.google.android.material.R.attr.colorOnSurface))
        background = MaterialShapeDrawable().apply {
            val surface = themeColor(com.google.android.material.R.attr.colorSurfaceContainerHigh)
            fillColor = android.content.res.ColorStateList.valueOf((surface and 0x00FFFFFF) or (0xE6 shl 24))
            setCornerSize(dp(12).toFloat())
        }
        setPadding(dp(12), dp(6), dp(12), dp(6))
        visibility = android.view.View.GONE
    }

    /**
     * Fits the game image. Portrait: centred in the space above the controls (ambient light fills
     * above and below it).
     * Landscape: between the two control columns, unless that would shrink it a lot (very wide
     * pictures such as the NDS side-by-side layout) - then full size, under the controls.
     */
    private fun layoutGame() {
        val w = root.width
        val h = root.height
        if (w == 0 || h == 0) return
        val aspect = videoWidth.toFloat() / videoHeight
        fun fit(availW: Float, availH: Float): Pair<Int, Int> =
            if (availW / availH > aspect) (availH * aspect).toInt() to availH.toInt()
            else availW.toInt() to (availW / aspect).toInt()
        val portrait = h > w
        // Portrait, in order of preference:
        //  1. the game at full width above the controls at their natural height;
        //  2. the same with the controls tightened a little (down to their minimum);
        //  3. (shorter screens, e.g. 16:9, with tall pictures like the NDS) the game at full width
        //     from the top and the controls over its lower part, DraStic-style, with a toggle to
        //     hide them.
        var overlapping = false
        if (portrait) {
            val natural = overlay.portraitControlHeight()
            val spare = h - w / aspect
            overlay.portraitArea = if (spare < natural && spare >= overlay.portraitMinimumHeight()) spare else 0f
            overlapping = spare < overlay.portraitMinimumHeight()
        }
        val gameArea = if (overlapping) h.toFloat() else h - overlay.portraitControlArea(h.toFloat())
        val (gw, gh) = if (portrait) {
            fit(w.toFloat(), gameArea)
        } else {
            val full = fit(w.toFloat(), h.toFloat())
            val beside = fit(w - 2 * overlay.landscapeColumnWidth(h), h.toFloat())
            if (beside.first >= full.first * 0.8f) {
                beside
            } else {
                overlapping = true
                full
            }
        }
        overlay.overlapping = overlapping
        val left = (w - gw) / 2
        val top = when {
            !portrait -> (h - gh) / 2
            overlapping -> 0
            else -> ((gameArea - gh) / 2).toInt()
        }
        NativeBridge.setGameRect(left, top, gw, gh)
        overlay.gameRect.set(left.toFloat(), top.toFloat(), (left + gw).toFloat(), (top + gh).toFloat())
        overlay.layoutControls()
    }

    private val isLandscape get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // Portrait and landscape have their own touch layouts. Rotating while editing keeps the
        // edits made so far and continues with the other orientation's layout.
        if (isLandscape != layoutLandscape) {
            if (editingLayout) touchLayouts.save(game.platform, layoutLandscape, overlay.placements)
            layoutLandscape = isLandscape
            overlay.placements = touchLayouts.load(game.platform, layoutLandscape)
            placementsBeforeEdit = overlay.placements
            if (editingLayout) {
                overlay.editMode = true  // drops the selection, which belonged to the old layout
                updateEditBar()
            }
        }
        // Each orientation has its own NDS screen layout (side by side suits landscape).
        if (started && game.platform == Platform.NDS) {
            NativeBridge.setOption("melonds_screen_layout1", settings.ndsLayout(isLandscape))
        }
        root.post { layoutGame() }
    }

    // ---- lifecycle ----

    override fun onResume() {
        super.onResume()
        resumed = true
        keyTable = Keymap(this).table()
        if (started && !menuOpen) NativeBridge.setPaused(false)
        if (settings.showStats) {
            statsLabel.visibility = android.view.View.VISIBLE
            root.removeCallbacks(statsUpdater)
            root.postDelayed(statsUpdater, 1000)
        }
    }

    override fun onPause() {
        super.onPause()
        resumed = false
        root.removeCallbacks(statsUpdater)
        if (started) NativeBridge.setPaused(true)  // also writes battery saves
    }

    override fun onDestroy() {
        super.onDestroy()
        if (NativeBridge.listener === this) NativeBridge.listener = null
        multicastLock?.takeIf { it.isHeld }?.release()
        wifiLock?.takeIf { it.isHeld }?.release()
        if (isFinishing && ::game.isInitialized) NativeBridge.stop()
    }

    // ---- surface ----

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeBridge.setViewSize(width, height)
        NativeBridge.setSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeBridge.setSurface(null)
    }

    // ---- native callbacks (emulation thread) ----

    override fun onVideoSize(width: Int, height: Int) {
        runOnUiThread {
            videoWidth = width
            videoHeight = height
            layoutGame()
        }
    }

    override fun onMessage(message: String) {
        // The cores speak English; translate the messages players are likely to see.
        val text = when {
            message.contains("VPN", ignoreCase = true) -> "VPN이나 터널(Hamachi 등)을 켜 두면 통신이 동작하지 않습니다."
            message.startsWith("Closed") -> "덮개를 닫았습니다 (다시 메뉴에서 열 수 있습니다)"
            message.startsWith("Opened") -> "덮개를 열었습니다"
            else -> message
        }
        runOnUiThread { toast(text) }
    }

    override fun onRumble(level: Int) {
        if (!settings.rumble || !vibrator.hasVibrator()) return
        overlay.hapticsSuppressed = level != 0
        if (level == 0) {
            vibrator.cancel()
        } else if (Build.VERSION.SDK_INT >= 26) {
            // Long one-shot, cancelled when the game turns the motor off.
            val amplitude = if (vibrator.hasAmplitudeControl()) level.coerceIn(1, 255) else android.os.VibrationEffect.DEFAULT_AMPLITUDE
            vibrator.vibrate(android.os.VibrationEffect.createOneShot(5000, amplitude))
        }
    }

    override fun onNetplayStatus(state: Int, players: Int, message: String) {
        runOnUiThread {
            netplayState = Netplay.state(state)
            netplayPlayers = players
            if (message.isNotEmpty()) toast(message)
            netplayLabel.text = netplayStatusText().let { if (it.isEmpty()) "" else "$linkShortName · $it" }
            netplayLabel.visibility = if (netplayState == Netplay.State.IDLE) android.view.View.GONE else android.view.View.VISIBLE
            if (netplayActive && speed != 1f) applySpeed(1f)
            // Hosts must receive broadcast discovery requests, which some Wi-Fi drivers filter
            // unless a multicast lock is held.
            if (netplayState == Netplay.State.HOSTING) {
                val lock = multicastLock ?: (applicationContext.getSystemService(WIFI_SERVICE) as WifiManager)
                    .createMulticastLock("emulate-netplay").apply { setReferenceCounted(false) }
                multicastLock = lock
                if (!lock.isHeld) lock.acquire()
            } else {
                multicastLock?.takeIf { it.isHeld }?.release()
            }
            // Every link transfer waits for a network round trip; keep Wi-Fi out of power save.
            if (netplayState != Netplay.State.IDLE) {
                val lock = wifiLock ?: (applicationContext.getSystemService(WIFI_SERVICE) as WifiManager).createWifiLock(
                    if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    else @Suppress("DEPRECATION") WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                    "emulate-netplay",
                ).apply { setReferenceCounted(false) }
                wifiLock = lock
                if (!lock.isHeld) lock.acquire()
            } else {
                wifiLock?.takeIf { it.isHeld }?.release()
            }
        }
    }

    private fun netplayStatusText() = when (netplayState) {
        Netplay.State.HOSTING -> "방장 · ${netplayPlayers}명"
        Netplay.State.JOINING -> "연결 중…"
        Netplay.State.CLIENT -> "연결됨"
        Netplay.State.IDLE -> ""
    }

    private val netplayActive get() = netplayState == Netplay.State.HOSTING || netplayState == Netplay.State.CLIENT

    // ---- touch controls ----

    override fun onButtons(mask: Int) {
        touchButtons = mask
        pushButtons()
    }

    override fun onPointer(x: Int, y: Int, pressed: Boolean) = NativeBridge.setPointer(x, y, pressed)

    override fun onMenu() = showMenu()

    override fun onFastForwardButton(pressed: Boolean) {
        if (settings.fastForwardHold) {
            setSpeed(if (pressed) settings.fastForwardSpeed else 1f)
        } else if (pressed) {
            toggleFastForward()
        }
    }

    /** Fast-forward button: back to normal from any other speed, else the fast-forward speed. */
    private fun toggleFastForward() = setSpeed(if (speed != 1f) 1f else settings.fastForwardSpeed)

    private fun setSpeed(value: Float) {
        if (value != 1f && netplayActive) {
            toast("$linkName 중에는 속도를 바꿀 수 없습니다")
            return
        }
        applySpeed(value)
    }

    private fun applySpeed(value: Float) {
        speed = value
        NativeBridge.setSpeed(value)
        overlay.showSpeed(if (value == 1f) null else Settings.speedLabel(value))
    }

    private fun updateStats() {
        if (!started) return
        val s = NativeBridge.getStats()
        val pacing = if (s[4] > 0f) "주사율 동기" else "타이머"
        val speedText = if (s[5] != 1f) " · ${Settings.speedLabel(s[5])}" else ""
        statsLabel.text = String.format(
            java.util.Locale.US,
            "게임 %.1f fps · 화면 %.1f fps\n%.0f Hz %s · 코어 %.1f ms%s",
            s[0], s[1], s[2], pacing, s[3], speedText,
        )
    }

    private fun pushButtons() = NativeBridge.setButtons(touchButtons or padButtons or padAxisButtons or pulseButtons)

    /** Holds [mask] for a moment, as if pressed and released. */
    private fun pulse(mask: Int) {
        pulseButtons = mask
        pushButtons()
        root.postDelayed({
            pulseButtons = 0
            pushButtons()
        }, 150)
    }

    // ---- physical controllers ----

    private fun onBackRequested() = if (editingLayout) confirmLeaveLayoutEdit() else showMenu()

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP) onBackRequested()
            return true
        }
        val action = keyTable[event.keyCode]
        if (action == null || menuOpen) return super.dispatchKeyEvent(event)
        if (action == PadAction.FAST_FORWARD && event.action == KeyEvent.ACTION_UP) {
            onFastForwardButton(false)
            return true
        }
        if (action.bit != 0) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> padButtons = padButtons or action.bit
                KeyEvent.ACTION_UP -> padButtons = padButtons and action.bit.inv()
            }
            pushButtons()
        } else if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            when (action) {
                PadAction.MENU -> showMenu()
                PadAction.FAST_FORWARD -> onFastForwardButton(true)
                PadAction.QUICK_SAVE -> quickState(save = true)
                PadAction.QUICK_LOAD -> quickState(save = false)
                else -> {}
            }
        }
        return true
    }

    private fun quickState(save: Boolean) {
        if (!started) return
        if (!save && netplayActive) {
            toast("$linkName 중에는 상태를 불러올 수 없습니다")
            return
        }
        val ok = if (save) saveStateSlot(1) else loadStateSlot(1)
        toast(when {
            save && ok -> "슬롯 1에 저장했습니다"
            save -> "저장하지 못했습니다"
            ok -> "슬롯 1을 불러왔습니다"
            else -> "불러오지 못했습니다"
        })
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val joystick = event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.isFromSource(InputDevice.SOURCE_GAMEPAD)
        if (!joystick || event.action != MotionEvent.ACTION_MOVE) return super.dispatchGenericMotionEvent(event)
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X).takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_X)
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y).takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_Y)
        var mask = 0
        if (x < -0.5f) mask = mask or Pad.LEFT
        if (x > 0.5f) mask = mask or Pad.RIGHT
        if (y < -0.5f) mask = mask or Pad.UP
        if (y > 0.5f) mask = mask or Pad.DOWN
        if (event.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f) mask = mask or Pad.L
        if (event.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f) mask = mask or Pad.R
        padAxisButtons = mask
        pushButtons()
        return true
    }

    // ---- menu ----

    private fun showMenu() {
        if (menuOpen || !started) return
        menuOpen = true
        NativeBridge.setPaused(true)

        val sheet = com.google.android.material.bottomsheet.BottomSheetDialog(this)
        var next: (() -> Unit)? = null  // runs once the sheet is gone
        sheet.setOnDismissListener {
            val action = next
            if (action != null) action() else closeMenu()
        }
        fun pick(action: () -> Unit) {
            next = action
            sheet.dismiss()
        }
        val context = sheet.context

        val content = android.widget.LinearLayout(context).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(0, 0, 0, dp(16))
        }
        content.addView(com.google.android.material.bottomsheet.BottomSheetDragHandleView(context))

        // Header: title and what's going on.
        content.addView(TextView(context).apply {
            text = game.title
            setTextAppearance(R.style.TextAppearance_Emulate_TitleLarge)
            setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurface))
            maxLines = 2
            ellipsize = android.text.TextUtils.TruncateAt.END
            setPadding(dp(24), 0, dp(24), 0)
        })
        val status = buildList {
            add(game.platform.label)
            if (netplayState != Netplay.State.IDLE) add("$linkShortName ${netplayStatusText()}")
            if (speed != 1f) add("속도 ${Settings.speedLabel(speed)}")
        }.joinToString(" · ")
        content.addView(TextView(context).apply {
            text = status
            setTextAppearance(R.style.TextAppearance_Emulate_BodyMedium)
            setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant))
            setPadding(dp(24), dp(2), dp(24), dp(16))
        })

        // Quick actions: big tonal buttons.
        val quick = android.widget.LinearLayout(context).apply {
            orientation = android.widget.LinearLayout.HORIZONTAL
            setPadding(dp(16), 0, dp(16), dp(8))
        }
        fun quickButton(label: String, icon: Int, action: () -> Unit) {
            quick.addView(MaterialButton(context, null, R.attr.emuTonalButtonStyle).apply {
                text = label
                setIconResource(icon)
                iconGravity = MaterialButton.ICON_GRAVITY_TOP
                iconPadding = dp(6)
                iconSize = dp(24)
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
                insetTop = 0
                insetBottom = 0
                minHeight = dp(80)
                setPadding(dp(4), dp(12), dp(4), dp(12))
                shapeAppearanceModel = shapeAppearanceModel.withCornerSize(dp(20).toFloat())
                setOnClickListener { pick(action) }
            }, android.widget.LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(4), 0, dp(4), 0)
            })
        }
        quickButton("계속", R.drawable.ic_play) { closeMenu() }
        quickButton("상태 저장", R.drawable.ic_save) { showSlots(save = true) }
        quickButton("불러오기", R.drawable.ic_history) { showSlots(save = false) }
        quickButton("속도 ${Settings.speedLabel(speed)}", R.drawable.ic_fast_forward) { showSpeeds() }
        content.addView(quick)

        // Everything else: a list.
        fun row(label: String, detail: String?, icon: Int, danger: Boolean = false, action: () -> Unit) {
            val color = MaterialColors.getColor(content,
                if (danger) androidx.appcompat.R.attr.colorError else com.google.android.material.R.attr.colorOnSurface)
            val item = android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                minimumHeight = dp(56)
                setPadding(dp(24), dp(8), dp(24), dp(8))
                setBackgroundResource(android.util.TypedValue().also {
                    context.theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true)
                }.resourceId)
                setOnClickListener { pick(action) }
            }
            item.addView(android.widget.ImageView(context).apply {
                setImageResource(icon)
                imageTintList = android.content.res.ColorStateList.valueOf(
                    if (danger) color else MaterialColors.getColor(content, com.google.android.material.R.attr.colorOnSurfaceVariant))
            }, android.widget.LinearLayout.LayoutParams(dp(24), dp(24)))
            val texts = android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.VERTICAL
                setPadding(dp(16), 0, 0, 0)
                addView(TextView(context).apply {
                    text = label
                    setTextAppearance(R.style.TextAppearance_Emulate_BodyLarge)
                    setTextColor(color)
                })
                if (detail != null) addView(TextView(context).apply {
                    text = detail
                    setTextAppearance(R.style.TextAppearance_Emulate_BodyMedium)
                    setTextColor(MaterialColors.getColor(content, com.google.android.material.R.attr.colorOnSurfaceVariant))
                })
            }
            item.addView(texts)
            content.addView(item, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        row(linkName, if (netplayState == Netplay.State.IDLE) "같은 Wi-Fi의 기기와 연결" else netplayStatusText(), R.drawable.ic_wifi) { showNetplay() }
        if (game.platform == Platform.NDS) {
            row("화면 배치", Settings.NDS_LAYOUTS[settings.ndsLayout(isLandscape)], R.drawable.ic_view_agenda) { showLayouts() }
            row("덮개 닫기 / 열기", null, R.drawable.ic_laptop) {
                // melonDS DS toggles the lid on Y while L2 is held.
                closeMenu()
                pulse(Pad.L2 or Pad.Y)
            }
        }
        val activeCheats = library.cheats(game).load().count { it.enabled }
        row("치트", if (activeCheats > 0) "${activeCheats}개 켜짐" else null, R.drawable.ic_code) { showCheats() }
        row("터치 버튼 배치 편집", null, R.drawable.ic_gamepad) { startLayoutEdit() }
        row("리셋", null, R.drawable.ic_refresh) { NativeBridge.reset(); closeMenu() }
        row("게임 종료", null, R.drawable.ic_close, danger = true) { finish() }

        sheet.setContentView(androidx.core.widget.NestedScrollView(context).apply { addView(content) })
        sheet.behavior.state = com.google.android.material.bottomsheet.BottomSheetBehavior.STATE_EXPANDED
        sheet.behavior.skipCollapsed = true
        sheet.behavior.maxWidth = dp(640)
        sheet.show()
    }

    // ---- cheats ----

    private fun applyCheats() {
        val codes = library.cheats(game).load().filter { it.enabled }.map { CheatStore.normalize(it.code) }
        NativeBridge.setCheats(codes.toTypedArray())
    }

    private fun showCheats() {
        val store = library.cheats(game)
        val cheats = store.load().toMutableList()
        if (cheats.isEmpty()) {
            showAddCheat()
            return
        }
        val checked = cheats.map { it.enabled }.toBooleanArray()
        MaterialAlertDialogBuilder(this)
            .setTitle("치트")
            .setMultiChoiceItems(cheats.map { it.name }.toTypedArray(), checked) { _, which, isChecked ->
                checked[which] = isChecked
            }
            .setPositiveButton("적용") { _, _ ->
                store.save(cheats.mapIndexed { i, c -> c.copy(enabled = checked[i]) })
                applyCheats()
                toast("치트를 적용했습니다. 끈 치트는 게임을 다시 시작해야 완전히 풀릴 수 있습니다.")
                closeMenu()
            }
            .setNeutralButton("추가…") { _, _ -> showAddCheat() }
            .setNegativeButton("삭제…") { _, _ ->
                MaterialAlertDialogBuilder(this)
                    .setTitle("삭제할 치트")
                    .setItems(cheats.map { it.name }.toTypedArray()) { _, which ->
                        cheats.removeAt(which)
                        store.save(cheats)
                        applyCheats()
                        closeMenu()
                    }
                    .setOnCancelListener { closeMenu() }
                    .show()
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun showAddCheat() {
        val name = TextInputEditText(this)
        val code = TextInputEditText(this).apply {
            minLines = 3
            gravity = Gravity.TOP or Gravity.START
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            typeface = android.graphics.Typeface.MONOSPACE
        }
        val form = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
            addView(outlinedField("이름", "예: 이상한사탕 99개", name))
            addView(outlinedField("코드", CheatStore.example(game.platform), code).apply {
                helperText = "치트는 세이브를 망가뜨릴 수 있습니다. 쓰기 전에 상태 저장을 해 두세요."
            }, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { topMargin = dp(12) })
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("치트 추가")
            .setView(form)
            .setPositiveButton("추가") { _, _ ->
                val text = code.text.toString()
                if (!CheatStore.looksValid(game.platform, text)) {
                    toast("코드 형식이 올바르지 않습니다")
                    closeMenu()
                    return@setPositiveButton
                }
                val store = library.cheats(game)
                val label = name.text.toString().trim().ifEmpty { CheatStore.normalize(text).take(17) }
                store.save(store.load() + Cheat(label, CheatStore.normalize(text), enabled = true))
                applyCheats()
                toast("'$label' 치트를 켰습니다")
                closeMenu()
            }
            .setNegativeButton("취소") { _, _ -> closeMenu() }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    /** Material 3 outlined text field around [input]. */
    private fun outlinedField(label: String, placeholder: String?, input: TextInputEditText) =
        TextInputLayout(this, null, com.google.android.material.R.attr.textInputOutlinedStyle).apply {
            hint = label
            placeholderText = placeholder
            addView(input, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }

    // ---- touch layout editing ----

    private fun buildEditBar() {
        fun button(label: String, styleAttr: Int = R.attr.emuTonalButtonStyle, onClick: () -> Unit) =
            MaterialButton(this, null, styleAttr).apply {
                text = label
                minWidth = 0
                minimumWidth = 0
                setPadding(dp(14), 0, dp(14), 0)
                setOnClickListener { onClick() }
            }
        fun row(vararg views: android.view.View) = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            views.forEach { view ->
                addView(view, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                    .apply { setMargins(dp(3), 0, dp(3), 0) })
            }
        }
        editLabel = TextView(this).apply {
            setTextAppearance(R.style.TextAppearance_Emulate_BodyMedium)
            gravity = Gravity.CENTER
            setPadding(dp(8), 0, dp(8), dp(6))
        }
        editSmaller = button("작게") { overlay.resizeSelected(-0.1f) }
        editLarger = button("크게") { overlay.resizeSelected(0.1f) }
        editHide = button("숨기기") { overlay.toggleSelectedHidden() }
        val moveBar = button("⇅") {
            // Move the toolbar out of the way (top <-> bottom).
            val lp = editBar.layoutParams as FrameLayout.LayoutParams
            lp.gravity = if (lp.gravity and Gravity.TOP == Gravity.TOP) Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            else Gravity.TOP or Gravity.CENTER_HORIZONTAL
            editBar.layoutParams = lp
        }
        editBar = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(dp(16), dp(14), dp(16), dp(10))
            addView(editLabel)
            addView(row(editSmaller, editLarger, editHide, moveBar))
            addView(row(
                button("기본값", R.attr.emuTextButtonStyle) { confirmResetLayout() },
                button("취소", R.attr.emuTextButtonStyle) { finishLayoutEdit(save = false) },
                button("완료", R.attr.emuFilledButtonStyle) { finishLayoutEdit(save = true) },
            ))
            visibility = android.view.View.GONE
            isClickable = true  // don't let taps fall through to the overlay
            minimumWidth = dp(320)  // keep the size steady as the label changes
            elevation = dp(6).toFloat()
        }
        root.addView(editBar, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.CENTER_HORIZONTAL,
        ).apply { setMargins(dp(12), dp(12), dp(12), dp(12)) })
        editLabel.setTextColor(themeColor(com.google.android.material.R.attr.colorOnSurface))
        editBar.background = MaterialShapeDrawable().apply {
            fillColor = android.content.res.ColorStateList.valueOf(themeColor(com.google.android.material.R.attr.colorSurfaceContainerHigh))
            setCornerSize(dp(28).toFloat())
        }
    }

    private fun elementLabel(element: TouchElement) =
        if (element == TouchElement.FACE && game.platform == Platform.NDS) "A·B·X·Y 버튼" else element.label

    private fun updateEditBar() {
        if (!::editBar.isInitialized) return
        val element = overlay.selected
        val placement = overlay.selectedPlacement
        editLabel.text = if (element == null || placement == null) {
            "${if (isLandscape) "가로" else "세로"} 화면 배치 · 버튼을 끌어서 옮기세요.\n누른 버튼은 크기를 바꾸거나 숨길 수 있습니다."
        } else {
            "${elementLabel(element)} · 크기 ${(placement.scale * 100).toInt()}%" + if (placement.hidden) " · 숨김" else ""
        }
        val enabled = element != null
        editSmaller.isEnabled = enabled && (placement?.scale ?: 1f) > 0.5f
        editLarger.isEnabled = enabled && (placement?.scale ?: 1f) < 2f
        editHide.isEnabled = enabled
        editHide.text = if (placement?.hidden == true) "보이기" else "숨기기"
    }

    private fun startLayoutEdit() {
        // The game stays paused (the menu counts as open) until editing ends.
        editingLayout = true
        placementsBeforeEdit = overlay.placements
        overlay.alpha = 1f
        overlay.editMode = true
        editBar.visibility = android.view.View.VISIBLE
        statsLabel.visibility = android.view.View.GONE
        updateEditBar()
    }

    private fun finishLayoutEdit(save: Boolean) {
        if (save) {
            touchLayouts.save(game.platform, layoutLandscape, overlay.placements)
            toast("터치 버튼 배치를 저장했습니다")
        } else {
            overlay.placements = placementsBeforeEdit
        }
        overlay.editMode = false
        overlay.alpha = settings.controlsOpacity
        editBar.visibility = android.view.View.GONE
        if (settings.showStats) statsLabel.visibility = android.view.View.VISIBLE
        editingLayout = false
        closeMenu()
    }

    private fun confirmLeaveLayoutEdit() {
        if (overlay.placements == placementsBeforeEdit) {
            finishLayoutEdit(save = false)
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("바꾼 배치를 저장할까요?")
            .setPositiveButton("저장") { _, _ -> finishLayoutEdit(save = true) }
            .setNegativeButton("저장 안 함") { _, _ -> finishLayoutEdit(save = false) }
            .setNeutralButton("계속 편집", null)
            .show()
    }

    private fun confirmResetLayout() {
        MaterialAlertDialogBuilder(this)
            .setTitle("기본 배치로 되돌릴까요?")
            .setMessage("${if (isLandscape) "가로" else "세로"} 화면의 버튼 위치·크기·숨김이 모두 처음 상태로 돌아갑니다. '완료'를 눌러야 저장됩니다.")
            .setPositiveButton("되돌리기") { _, _ ->
                overlay.placements = emptyMap()
                overlay.editMode = true  // clear the selection
                updateEditBar()
            }
            .setNegativeButton("취소", null)
            .show()
    }

    private fun showSpeeds() {
        val speeds = Settings.GAME_SPEEDS
        val labels = speeds.map { value ->
            Settings.speedLabel(value) + when {
                value == 1f -> "  보통"
                value < 1f -> "  느리게"
                value == settings.fastForwardSpeed -> "  (빨리 감기 버튼)"
                else -> ""
            }
        }.toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle("게임 속도")
            .setSingleChoiceItems(labels, speeds.indexOf(speed)) { dialog, which ->
                val value = speeds[which]
                // A chosen fast speed also becomes what the fast-forward button uses.
                if (value > 1f) settings.fastForwardSpeed = value
                setSpeed(value)
                dialog.dismiss()
                closeMenu()
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun showLayouts() {
        val landscape = isLandscape
        val keys = Settings.NDS_LAYOUTS.keys.toList()
        val current = keys.indexOf(settings.ndsLayout(landscape))
        MaterialAlertDialogBuilder(this)
            .setTitle(if (landscape) "화면 배치 (가로 화면)" else "화면 배치 (세로 화면)")
            .setSingleChoiceItems(Settings.NDS_LAYOUTS.values.toTypedArray(), current) { dialog, which ->
                settings.setNdsLayout(landscape, keys[which])
                NativeBridge.setOption("melonds_screen_layout1", keys[which])
                dialog.dismiss()
                closeMenu()
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private val stateSlots = 1..8

    private fun saveStateSlot(slot: Int): Boolean {
        val ok = NativeBridge.saveState(library.statePath(game, slot).absolutePath)
        if (ok) writeThumbnail(library.stateThumbnailPath(game, slot))
        return ok
    }

    private fun loadStateSlot(slot: Int) = NativeBridge.loadState(library.statePath(game, slot).absolutePath)

    /** Saves the current picture next to a state so the slot list can show it. */
    private fun writeThumbnail(file: java.io.File) {
        val data = NativeBridge.captureFrame()
        if (data == null || data.size < 2) {
            file.delete()  // never show a picture from an older state
            return
        }
        val width = data[0]
        val height = data[1]
        val bitmap = android.graphics.Bitmap.createBitmap(data, 2, width, width, height, android.graphics.Bitmap.Config.ARGB_8888)
        val tmp = java.io.File(file.path + ".tmp")
        runCatching {
            tmp.outputStream().use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
            if (!tmp.renameTo(file)) error("rename failed")
        }.onFailure {
            tmp.delete()
            file.delete()
        }
        bitmap.recycle()
    }

    private fun showSlots(save: Boolean) {
        if (!save && netplayActive) {
            toast("$linkName 중에는 상태를 불러올 수 없습니다")
            closeMenu()
            return
        }
        val builder = MaterialAlertDialogBuilder(this).setTitle(if (save) "상태 저장" else "상태 불러오기")
        val context = builder.context
        val density = resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()
        val format = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
        val rowBackground = android.util.TypedValue().also {
            context.theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true)
        }.resourceId
        var dialog: AlertDialog? = null

        val list = android.widget.LinearLayout(context).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(0, dp(4), 0, dp(4))
        }
        list.addView(TextView(context).apply {
            text = if (save) "고른 슬롯에 지금 상태를 저장합니다. 길게 누르면 삭제합니다." else "길게 누르면 삭제합니다."
            textSize = 12f
            alpha = 0.7f
            setPadding(dp(24), 0, dp(24), dp(8))
        })
        for (slot in stateSlots) {
            val exists = library.statePath(game, slot).exists()
            val thumbnail = library.stateThumbnailPath(game, slot).takeIf { exists && it.exists() }
                ?.let { android.graphics.BitmapFactory.decodeFile(it.path) }
            val image = android.widget.ImageView(context).apply {
                scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
                if (thumbnail != null) {
                    // Keep pixels crisp when the small picture is scaled up.
                    setImageDrawable(android.graphics.drawable.BitmapDrawable(resources, thumbnail).apply { isFilterBitmap = false })
                }
            }
            val box = FrameLayout(context).apply {
                background = MaterialShapeDrawable().apply {
                    fillColor = android.content.res.ColorStateList.valueOf(
                        MaterialColors.getColor(list, com.google.android.material.R.attr.colorSurfaceContainerHighest))
                    setCornerSize(dp(12).toFloat())
                }
                clipToOutline = true
                addView(image, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
                if (thumbnail == null) {
                    addView(TextView(context).apply {
                        text = if (exists) "미리보기\n없음" else "비어 있음"
                        textSize = 11f
                        alpha = 0.6f
                        gravity = Gravity.CENTER
                    }, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
                }
            }
            val texts = android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.VERTICAL
                setPadding(dp(16), 0, 0, 0)
                addView(TextView(context).apply {
                    text = if (slot == 1) "슬롯 1  (빠른 저장)" else "슬롯 $slot"
                    textSize = 16f
                    setTypeface(typeface, android.graphics.Typeface.BOLD)
                })
                addView(TextView(context).apply {
                    text = if (exists) format.format(Date(library.statePath(game, slot).lastModified())) else "비어 있음"
                    textSize = 13f
                    alpha = 0.7f
                })
            }
            val row = android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(24), dp(6), dp(24), dp(6))
                setBackgroundResource(rowBackground)
                addView(box, android.widget.LinearLayout.LayoutParams(dp(96), dp(72)))
                addView(texts)
                // Nothing to load from an empty slot.
                isEnabled = save || exists
                if (!isEnabled) alpha = 0.45f
                setOnClickListener {
                    dialog?.dismiss()
                    val ok = if (save) saveStateSlot(slot) else loadStateSlot(slot)
                    toast(when {
                        save && ok -> "슬롯 ${slot}에 저장했습니다"
                        save -> "저장하지 못했습니다"
                        ok -> "슬롯 ${slot}을 불러왔습니다"
                        else -> "불러오지 못했습니다"
                    })
                    closeMenu()
                }
                if (exists) setOnLongClickListener {
                    MaterialAlertDialogBuilder(this@EmulatorActivity)
                        .setTitle("슬롯 $slot 삭제")
                        .setMessage("이 슬롯의 상태 저장을 지울까요? 게임의 세이브(배터리 세이브)는 지워지지 않습니다.")
                        .setPositiveButton("삭제") { _, _ ->
                            library.deleteState(game, slot)
                            dialog?.dismiss()
                            showSlots(save)  // reopen with the slot emptied (the menu stays open)
                        }
                        .setNegativeButton("취소", null)
                        .show()
                    true
                }
            }
            list.addView(row, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        dialog = builder
            .setView(android.widget.ScrollView(context).apply { addView(list) })
            .setNegativeButton("닫기") { _, _ -> closeMenu() }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    // ---- NDS local wireless / GB-GBA link cable ----

    private val linkName get() = if (game.platform == Platform.NDS) "무선 통신" else "통신 케이블"
    private val linkShortName get() = if (game.platform == Platform.NDS) "무선" else "케이블"

    private fun showNetplay() {
        val builder = MaterialAlertDialogBuilder(this).setTitle(linkName)
        val addresses = Netplay.localAddresses().joinToString(", ") { it.address.hostAddress ?: "" }
            .ifEmpty { "없음 (Wi-Fi에 연결되어 있지 않음)" }
        val intro = when (game.platform) {
            Platform.NDS -> "같은 Wi-Fi(또는 한쪽 폰의 핫스팟)에 연결된 기기끼리 DS 근거리 무선 통신을 할 수 있습니다."
            Platform.GBA -> "같은 Wi-Fi(또는 한쪽 폰의 핫스팟)에 연결된 기기끼리 GBA 통신 케이블로 연결한 것처럼 " +
                "교환·대전을 할 수 있습니다(최대 4명). 방을 만든 기기가 1P가 됩니다."
            else -> "같은 Wi-Fi(또는 한쪽 폰의 핫스팟)에 연결된 기기끼리 통신 케이블로 연결한 것처럼 " +
                "교환·대전을 할 수 있습니다(2명). GB와 GBC 게임끼리도 연결됩니다."
        }
        val identity = if (game.platform == Platform.NDS) "\nDS MAC: ${Netplay.macAddress(this)}" else ""
        when (netplayState) {
            Netplay.State.IDLE -> builder
                .setMessage(
                    "$intro\n\n" +
                        "1. 두 기기 모두 이 앱에서 게임을 실행합니다.\n" +
                        "2. 한 기기에서 '방 만들기'를 누릅니다.\n" +
                        "3. 다른 기기에서 '방 찾기'로 참가합니다.\n" +
                        "4. 연결되면 게임 안에서 평소처럼 통신(교환, 대전 등)을 시작하세요.\n\n" +
                        "이 기기 주소: $addresses$identity"
                )
                .setPositiveButton("방 만들기") { _, _ -> hostRoom() }
                .setNeutralButton("방 찾기") { _, _ -> findRooms() }
                .setNegativeButton("IP로 참가") { _, _ -> joinByAddress() }
            Netplay.State.HOSTING -> builder
                .setMessage("방을 열었습니다.\n\n이 기기 주소: $addresses\n참가 인원: ${netplayPlayers}명 (나 포함)\n\n" +
                    "다른 기기에서 '방 찾기'를 누르거나 위 주소로 참가하세요.")
                .setPositiveButton("닫기") { _, _ -> closeMenu() }
                .setNegativeButton("통신 종료") { _, _ -> NativeBridge.netplayLeave(); closeMenu() }
            Netplay.State.JOINING, Netplay.State.CLIENT -> builder
                .setMessage(if (netplayState == Netplay.State.CLIENT) "호스트에 연결되어 있습니다." else "호스트에 연결하는 중입니다…")
                .setPositiveButton("닫기") { _, _ -> closeMenu() }
                .setNegativeButton("통신 종료") { _, _ -> NativeBridge.netplayLeave(); closeMenu() }
        }
        builder.setOnCancelListener { closeMenu() }.show()
    }

    private fun hostRoom() {
        NativeBridge.netplayHost(game.title, Netplay.tag(game.platform))?.let { toast(it) }
        closeMenu()
    }

    private fun findRooms() {
        val searching = MaterialAlertDialogBuilder(this)
            .setTitle("방 찾는 중…")
            .setView(FrameLayout(this).apply {
                setPadding(0, dp(24), 0, dp(24))
                addView(CircularProgressIndicator(context).apply { isIndeterminate = true },
                    FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
            })
            .setCancelable(false)
            .show()
        thread(name = "netplay-scan") {
            val all = runCatching { Netplay.scan() }.getOrDefault(emptyList())
            val hosts = all.filter { it.tag == Netplay.tag(game.platform) }
            runOnUiThread {
                searching.dismiss()
                if (isFinishing) return@runOnUiThread
                if (hosts.isEmpty()) {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("방을 찾지 못했습니다")
                        .setMessage(
                            (if (all.isNotEmpty()) "다른 기종 게임의 방만 있습니다 (${all.joinToString { it.title }}).\n\n" else "") +
                                "상대 기기에서 '방 만들기'를 했는지, 두 기기가 같은 Wi-Fi에 있는지 확인하세요. " +
                                "공용 Wi-Fi는 기기 간 통신을 막는 경우가 많습니다. 이럴 땐 한쪽 폰의 핫스팟을 이용해 보세요."
                        )
                        .setPositiveButton("다시 찾기") { _, _ -> findRooms() }
                        .setNeutralButton("IP로 참가") { _, _ -> joinByAddress() }
                        .setNegativeButton("닫기") { _, _ -> closeMenu() }
                        .setOnCancelListener { closeMenu() }
                        .show()
                    return@runOnUiThread
                }
                val labels = hosts.map { "${it.title}\n${it.address} · ${it.players}/${it.maxPlayers}명" }.toTypedArray()
                MaterialAlertDialogBuilder(this)
                    .setTitle("참가할 방")
                    .setItems(labels) { _, which -> joinRoom(hosts[which].address) }
                    .setNeutralButton("다시 찾기") { _, _ -> findRooms() }
                    .setOnCancelListener { closeMenu() }
                    .show()
            }
        }
    }

    private fun joinByAddress() {
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        val input = TextInputEditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setText(prefs.getString("last_host", ""))
        }
        val field = android.widget.FrameLayout(this).apply {
            setPadding(dp(24), dp(8), dp(24), 0)
            addView(outlinedField("IP 주소", "예: 192.168.0.12", input))
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("호스트 IP 주소")
            .setView(field)
            .setPositiveButton("참가") { _, _ ->
                val address = input.text.toString().trim()
                if (address.isEmpty()) {
                    closeMenu()
                    return@setPositiveButton
                }
                prefs.edit().putString("last_host", address).apply()
                joinRoom(address)
            }
            .setNegativeButton("취소") { _, _ -> closeMenu() }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun joinRoom(address: String) {
        NativeBridge.netplayJoin(address, Netplay.tag(game.platform))?.let { toast(it) }
        closeMenu()
    }

    private fun closeMenu() {
        menuOpen = false
        if (resumed) NativeBridge.setPaused(false)
    }

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
}
