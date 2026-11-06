package com.emulate.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeTopAppBar
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.painterResource
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

class SettingsActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val settings = Settings(this)
        setContent {
            EmulateTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    var keymapOpen by remember { mutableStateOf(false) }
                    if (keymapOpen) {
                        KeymapScreen(Keymap(this), onBack = { keymapOpen = false })
                    } else {
                        SettingsScreen(settings, onBack = { finish() }, onKeymap = { keymapOpen = true })
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun SettingsScreen(settings: Settings, onBack: () -> Unit, onKeymap: () -> Unit) {
    val context = LocalContext.current
    var opacity by remember { mutableFloatStateOf(settings.controlsOpacity) }
    var scale by remember { mutableFloatStateOf(settings.controlsScale) }
    var haptics by remember { mutableStateOf(settings.haptics) }
    var rumble by remember { mutableStateOf(settings.rumble) }
    var audio by remember { mutableStateOf(settings.audio) }
    var ffSpeed by remember { mutableFloatStateOf(settings.fastForwardSpeed) }
    var palette by remember { mutableStateOf(settings.gbPalette) }
    var paletteOpen by remember { mutableStateOf(false) }
    var filter by remember { mutableStateOf(settings.videoFilter) }
    var filterOpen by remember { mutableStateOf(false) }
    var vsync by remember { mutableStateOf(settings.vsync) }
    var showStats by remember { mutableStateOf(settings.showStats) }
    var ambient by remember { mutableStateOf(settings.ambient) }
    var ffHold by remember { mutableStateOf(settings.fastForwardHold) }
    var ffAudio by remember { mutableStateOf(settings.fastForwardAudio) }
    val library = remember { GameLibrary(context) }
    var biosFiles by remember { mutableStateOf(library.ndsSystemFiles()) }
    var biosOpen by remember { mutableStateOf(false) }
    val touchLayouts = remember { TouchLayoutStore(context) }
    var layoutResetOpen by remember { mutableStateOf(false) }
    val biosPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        val stored = uris.mapNotNull { library.importNdsSystemFile(it) }
        biosFiles = library.ndsSystemFiles()
        val message = buildString {
            if (stored.isNotEmpty()) append("가져왔습니다: ${stored.distinct().joinToString()}")
            if (stored.size < uris.size) {
                if (isNotEmpty()) append("\n")
                append("${uris.size - stored.size}개는 DS BIOS·펌웨어 파일이 아닙니다 (크기가 맞지 않음).")
            }
        }
        android.widget.Toast.makeText(context, message, android.widget.Toast.LENGTH_LONG).show()
    }

    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            LargeTopAppBar(
                title = { Text("설정") },
                scrollBehavior = scrollBehavior,
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "뒤로") }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState())) {
            Section("터치 컨트롤")
            SliderRow("버튼 불투명도", "${(opacity * 100).roundToInt()}%", opacity, 0.1f..1f) {
                opacity = it
                settings.controlsOpacity = it
            }
            SliderRow("버튼 크기", "${(scale * 100).roundToInt()}%", scale, 0.7f..1.4f) {
                scale = it
                settings.controlsScale = it
            }
            SwitchRow("누를 때 진동", "버튼을 누르고 뗄 때 '딸깍' 하는 짧은 진동을 줍니다. 리니어 모터를 쓰는 폰에서 가장 또렷합니다.", haptics) {
                haptics = it
                settings.haptics = it
            }
            ClickRow(
                "버튼 배치 초기화",
                "버튼 위치·크기·숨김은 게임 중 메뉴의 '터치 버튼 배치 편집'에서 바꿉니다 " +
                    "(게임기와 화면 방향마다 따로 저장). 여기서는 모두 처음 상태로 되돌립니다.",
            ) { layoutResetOpen = true }

            Section("화면")
            ClickRow("화면 필터", Settings.VIDEO_FILTERS[filter] ?: filter) { filterOpen = true }
            SwitchRow(
                "화면 주사율에 맞추기",
                "게임을 디스플레이 주사율(60·120Hz)에 맞춰 돌려 화면이 매끄럽게 움직입니다. " +
                    "화면이 끊기거나 소리가 이상하면 꺼 보세요.",
                vsync,
            ) {
                vsync = it
                settings.vsync = it
            }
            SwitchRow(
                "주변 빛 번짐",
                "게임 화면 가장자리의 색이 버튼 뒤 배경으로 은은하게 번져, 화면이 바뀌면 함께 바뀝니다.",
                ambient,
            ) {
                ambient = it
                settings.ambient = it
            }
            SwitchRow("성능 정보 표시", "게임 중 화면 위쪽에 초당 프레임 수와 처리 시간을 표시합니다.", showStats) {
                showStats = it
                settings.showStats = it
            }

            Section("게임 속도")
            Column(Modifier.padding(horizontal = 20.dp, vertical = 12.dp)) {
                Text("빨리 감기 속도", style = MaterialTheme.typography.bodyLarge)
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    modifier = Modifier.padding(top = 8.dp),
                ) {
                    Settings.FAST_FORWARD_SPEEDS.forEach { speed ->
                        FilterChip(
                            selected = ffSpeed == speed,
                            onClick = {
                                ffSpeed = speed
                                settings.fastForwardSpeed = speed
                            },
                            label = { Text(Settings.speedLabel(speed)) },
                        )
                    }
                }
                Text(
                    "게임 중 메뉴의 '게임 속도'에서 느리게(×0.5, ×0.75)도 고를 수 있습니다.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            SwitchRow("누르고 있는 동안만 빨리 감기", "끄면 ▶▶ 버튼을 누를 때마다 켜고 끕니다.", ffHold) {
                ffHold = it
                settings.fastForwardHold = it
            }
            SwitchRow("빨리 감기 중 소리", "음높이는 그대로 두고 빠르게 재생합니다. 느리게 할 때도 음높이를 유지합니다. 끄면 빨리 감기 중에만 소리를 끕니다.", ffAudio) {
                ffAudio = it
                settings.fastForwardAudio = it
            }

            Section("게임패드 · 키보드")
            ClickRow("버튼 설정", "블루투스·USB 게임패드나 키보드의 버튼을 바꿉니다. 메뉴·빨리 감기·빠른 저장 단축키도 정할 수 있습니다.", onKeymap)

            Section("게임")
            SwitchRow("소리", null, audio) {
                audio = it
                settings.audio = it
            }
            SwitchRow("진동(럼블) 지원", "포켓몬 핀볼, 드릴 도저 등 진동 카트리지 게임에서 폰이 진동합니다.", rumble) {
                rumble = it
                settings.rumble = it
            }
            ClickRow("게임보이 화면 색상", Settings.GB_PALETTES[palette] ?: palette) { paletteOpen = true }

            Section("NDS")
            val biosCount = biosFiles.count { it.value }
            ClickRow(
                "BIOS · 펌웨어 (선택 사항)",
                when (biosCount) {
                    3 -> "실제 DS의 BIOS·펌웨어를 사용합니다"
                    0 -> "없음 · 내장 대체 BIOS 사용 (대부분의 게임은 이걸로 충분합니다)"
                    else -> "$biosCount/3개 · 3개가 모두 있어야 사용됩니다"
                },
            ) { biosOpen = true }

            Section("정보")
            Text(
                "DS 무선 통신용 MAC 주소: ${Netplay.macAddress(context)}\n" +
                    "설정은 다음에 게임을 시작할 때 적용됩니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
        }
    }

    if (layoutResetOpen) {
        AlertDialog(
            onDismissRequest = { layoutResetOpen = false },
            title = { Text("버튼 배치 초기화") },
            text = { Text("모든 게임기와 화면 방향의 터치 버튼 배치를 처음 상태로 되돌릴까요?") },
            confirmButton = {
                TextButton(onClick = {
                    touchLayouts.clearAll()
                    layoutResetOpen = false
                    android.widget.Toast.makeText(context, "버튼 배치를 초기화했습니다", android.widget.Toast.LENGTH_SHORT).show()
                }) { Text("초기화") }
            },
            dismissButton = { TextButton(onClick = { layoutResetOpen = false }) { Text("취소") } },
        )
    }

    if (filterOpen) {
        AlertDialog(
            onDismissRequest = { filterOpen = false },
            title = { Text("화면 필터") },
            text = {
                Column {
                    Settings.VIDEO_FILTERS.forEach { (key, label) ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                filter = key
                                settings.videoFilter = key
                                filterOpen = false
                            }.padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = filter == key, onClick = null)
                            Text(label, modifier = Modifier.padding(start = 8.dp))
                        }
                    }
                    Text(
                        "선명하게: 픽셀 크기를 고르게 맞춘 또렷한 화면\n원본 픽셀: 보정 없는 그대로의 픽셀\n" +
                            "부드럽게: 계단 현상을 흐리게 다듬은 화면\nLCD 격자: 휴대용 게임기 액정처럼 픽셀 사이에 옅은 선",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
            },
            confirmButton = { TextButton(onClick = { filterOpen = false }) { Text("닫기") } },
        )
    }

    if (biosOpen) {
        AlertDialog(
            onDismissRequest = { biosOpen = false },
            title = { Text("NDS BIOS · 펌웨어") },
            text = {
                Column {
                    Text(
                        "직접 가진 DS에서 추출한 bios7.bin, bios9.bin, firmware.bin을 넣으면 내장 대체 BIOS 대신 " +
                            "실제 BIOS를 씁니다. 대체 BIOS에서 문제가 있는 게임에만 필요합니다. " +
                            "파일 크기로 종류를 알아보니 이름은 달라도 됩니다.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    GameLibrary.NDS_SYSTEM_FILES.forEach { name ->
                        Text(
                            (if (biosFiles[name] == true) "✓ " else "✗ ") + name,
                            modifier = Modifier.padding(top = 6.dp),
                        )
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { biosPicker.launch(arrayOf("*/*")) }) { Text("파일 선택…") }
            },
            dismissButton = {
                Row {
                    if (biosFiles.values.any { it }) {
                        TextButton(onClick = {
                            library.deleteNdsSystemFiles()
                            biosFiles = library.ndsSystemFiles()
                        }) { Text("삭제") }
                    }
                    TextButton(onClick = { biosOpen = false }) { Text("닫기") }
                }
            },
        )
    }

    if (paletteOpen) {
        AlertDialog(
            onDismissRequest = { paletteOpen = false },
            title = { Text("게임보이 화면 색상") },
            text = {
                Column {
                    Text(
                        "흑백 게임보이용 게임(GBC 전용이 아닌 게임)에 적용됩니다.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    Settings.GB_PALETTES.forEach { (key, label) ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                palette = key
                                settings.gbPalette = key
                                paletteOpen = false
                            }.padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = palette == key, onClick = null)
                            Text(label, modifier = Modifier.padding(start = 8.dp))
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { paletteOpen = false }) { Text("닫기") } },
        )
    }
}

@Composable
private fun Section(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp, bottom = 4.dp),
    )
}

private val transparentItem @Composable get() = ListItemDefaults.colors(containerColor = Color.Transparent)

@Composable
private fun SliderRow(title: String, value: String, current: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit) {
    Column(Modifier.padding(horizontal = 24.dp, vertical = 8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(value, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
        }
        Slider(value = current, onValueChange = onChange, valueRange = range)
    }
}

@Composable
private fun SwitchRow(title: String, subtitle: String?, checked: Boolean, onChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = subtitle?.let { { Text(it) } },
        trailingContent = { Switch(checked = checked, onCheckedChange = onChange) },
        colors = transparentItem,
        modifier = Modifier.clickable { onChange(!checked) }.padding(horizontal = 8.dp),
    )
}

@Composable
private fun ClickRow(title: String, value: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(value) },
        colors = transparentItem,
        modifier = Modifier.clickable(onClick = onClick).padding(horizontal = 8.dp),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun KeymapScreen(keymap: Keymap, onBack: () -> Unit) {
    val context = LocalContext.current
    var version by remember { mutableIntStateOf(0) }  // bump to re-read the key names
    androidx.activity.compose.BackHandler(onBack = onBack)

    fun capture(action: PadAction) {
        val dialog = com.google.android.material.dialog.MaterialAlertDialogBuilder(context)
            .setTitle("'${action.label}'에 쓸 버튼")
            .setMessage("게임패드나 키보드에서 버튼을 누르세요.\n(뒤로 가기 키는 쓸 수 없습니다)")
            .setNegativeButton("취소", null)
            .setNeutralButton("지우기") { _, _ ->
                keymap.clear(action)
                version++
            }
            .create()
        dialog.setOnKeyListener { d, keyCode, event ->
            if (keyCode == android.view.KeyEvent.KEYCODE_BACK) return@setOnKeyListener false
            if (event.action == android.view.KeyEvent.ACTION_DOWN) {
                keymap.assign(action, keyCode)
                version++
                d.dismiss()
            }
            true
        }
        dialog.show()
    }

    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            LargeTopAppBar(
                title = { Text("버튼 설정") },
                scrollBehavior = scrollBehavior,
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "뒤로") }
                },
                actions = {
                    TextButton(onClick = {
                        keymap.reset()
                        version++
                    }) { Text("기본값") }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState())) {
            Text(
                "항목을 누른 뒤 쓸 버튼을 누르세요. 아날로그 스틱과 십자 키 축은 항상 방향키로 쓰입니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
            version.let { // re-read after every change
                PadAction.entries.forEach { action ->
                    ClickRow(action.label, Keymap.keyName(keymap.keyFor(action))) { capture(action) }
                }
            }
        }
    }
}
