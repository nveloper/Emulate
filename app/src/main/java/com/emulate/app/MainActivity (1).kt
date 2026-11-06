package com.emulate.app

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeTopAppBar
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val library = GameLibrary(this)
        setContent {
            EmulateTheme {
                LibraryScreen(library)
            }
        }
    }
}

/** Tile colours per platform, from the current (wallpaper) colour scheme. */
@Composable
private fun platformColors(platform: Platform): Pair<Color, Color> = MaterialTheme.colorScheme.run {
    when (platform) {
        Platform.GB -> tertiaryContainer to onTertiaryContainer
        Platform.GBC -> secondaryContainer to onSecondaryContainer
        Platform.GBA -> primaryContainer to onPrimaryContainer
        Platform.NDS -> surfaceContainerHighest to onSurfaceVariant
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LibraryScreen(library: GameLibrary) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var games by remember { mutableStateOf(emptyList<Game>()) }
    var filter by remember { mutableStateOf<Platform?>(null) }
    var importing by remember { mutableIntStateOf(0) }
    var menuGame by remember { mutableStateOf<Game?>(null) }
    var infoGame by remember { mutableStateOf<Game?>(null) }
    var slot2For by remember { mutableStateOf<Game?>(null) }
    var deleteGame by remember { mutableStateOf<Game?>(null) }
    var refresh by remember { mutableIntStateOf(0) }
    var overflowOpen by remember { mutableStateOf(false) }
    var saveTarget by remember { mutableStateOf<Game?>(null) }  // game whose save is being imported/exported
    var confirmSaveImport by remember { mutableStateOf<Game?>(null) }
    var confirmRestore by remember { mutableStateOf(false) }

    fun toast(message: String) = Toast.makeText(context, message, Toast.LENGTH_LONG).show()

    /** Runs file work off the main thread, then reports the result. */
    fun work(task: () -> String) {
        scope.launch {
            val message = withContext(Dispatchers.IO) { runCatching(task).getOrElse { "실패했습니다: ${it.message}" } }
            toast(message)
            refresh++
        }
    }

    val saveImporter = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val game = saveTarget ?: return@rememberLauncherForActivityResult
        if (uri != null) work { library.importSave(game, uri) ?: "'${game.title}' 세이브를 가져왔습니다." }
    }
    val saveExporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val game = saveTarget ?: return@rememberLauncherForActivityResult
        if (uri != null) work { library.exportSave(game, uri) ?: "'${game.title}' 세이브를 내보냈습니다." }
    }
    val backupWriter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) work { "파일 ${library.backupAll(uri)}개를 백업했습니다." }
    }
    val backupReader = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) work { "파일 ${library.restoreBackup(uri)}개를 복원했습니다." }
    }

    LaunchedEffect(refresh) {
        games = withContext(Dispatchers.IO) { library.games() }
    }
    LifecycleResumeEffect(Unit) {
        refresh++
        onPauseOrDispose {}
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris: List<Uri> ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            importing = uris.size
            var skipped = 0
            var added = 0
            for (uri in uris) {
                val imported = withContext(Dispatchers.IO) { runCatching { library.import(uri) }.getOrDefault(emptyList()) }
                if (imported.isEmpty()) skipped++
                added += imported.size
                importing--
            }
            if (skipped > 0) {
                toast("지원하지 않는 파일 ${skipped}개를 건너뛰었습니다 (.gb .gbc .gba .nds 또는 이를 담은 .zip만 가능)")
            } else if (added > uris.size) {
                toast("게임 ${added}개를 추가했습니다")
            }
            refresh++
        }
    }

    val scrollBehavior = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    val listState = rememberLazyListState()
    val fabExpanded by remember { derivedStateOf { listState.firstVisibleItemIndex == 0 } }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            LargeTopAppBar(
                title = { Text("Emulate") },
                actions = {
                    IconButton(onClick = { context.startActivity(Intent(context, SettingsActivity::class.java)) }) {
                        Icon(painterResource(R.drawable.ic_settings), contentDescription = "설정")
                    }
                    Box {
                        IconButton(onClick = { overflowOpen = true }) {
                            Icon(painterResource(R.drawable.ic_more_vert), contentDescription = "더 보기")
                        }
                        DropdownMenu(expanded = overflowOpen, onDismissRequest = { overflowOpen = false }) {
                            DropdownMenuItem(
                                text = { Text("세이브 전체 백업…") },
                                leadingIcon = { Icon(painterResource(R.drawable.ic_backup), null) },
                                onClick = {
                                    overflowOpen = false
                                    backupWriter.launch("emulate-backup.zip")
                                },
                            )
                            DropdownMenuItem(
                                text = { Text("백업에서 복원…") },
                                leadingIcon = { Icon(painterResource(R.drawable.ic_restore), null) },
                                onClick = {
                                    overflowOpen = false
                                    confirmRestore = true
                                },
                            )
                        }
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { picker.launch(arrayOf("*/*")) },
                expanded = fabExpanded,
                icon = { Icon(painterResource(R.drawable.ic_add), null) },
                text = { Text("ROM 추가") },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (importing > 0) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            LazyRow(
                contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                item { FilterChip(selected = filter == null, onClick = { filter = null }, label = { Text("전체") }) }
                items(Platform.entries) { p ->
                    FilterChip(selected = filter == p, onClick = { filter = p }, label = { Text(p.shortLabel) })
                }
            }

            val visible = remember(games, filter) { games.filter { filter == null || it.platform == filter } }
            if (visible.isEmpty()) {
                EmptyState(filtered = games.isNotEmpty(), onAdd = { picker.launch(arrayOf("*/*")) })
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(top = 8.dp, bottom = 96.dp),  // room for the FAB
                ) {
                    items(visible, key = { it.id }) { game ->
                        GameRow(
                            game = game,
                            onClick = { context.startActivity(EmulatorActivity.intent(context, game)) },
                            onLongClick = { menuGame = game },
                        )
                    }
                }
            }
        }
    }

    menuGame?.let { game ->
        ModalBottomSheet(onDismissRequest = { menuGame = null }) {
            Column(Modifier.navigationBarsPadding().padding(bottom = 16.dp)) {
                ListItem(
                    headlineContent = { Text(game.title, style = MaterialTheme.typography.titleLarge) },
                    supportingContent = { Text(game.platform.label) },
                    leadingContent = { GameIcon(game) },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                )
                Spacer(Modifier.height(8.dp))
                fun close() { menuGame = null }
                SheetAction(R.drawable.ic_info, "정보 · 이름 바꾸기",
                    if (game.hasAlias) "원래 이름: ${game.originalTitle}" else "ROM 정보를 보고 목록에 보일 이름을 정합니다") {
                    close()
                    infoGame = game
                }
                if (game.platform == Platform.NDS) {
                    SheetAction(R.drawable.ic_sd_card, "GBA 슬롯(Slot-2)에 카트리지 꽂기", game.slot2Title?.let { "지금: $it" }) {
                        close()
                        slot2For = game
                    }
                }
                SheetAction(R.drawable.ic_download, "세이브 가져오기", "실기 추출 .sav, 다른 에뮬레이터의 .sav/.srm, .dsv") {
                    close()
                    if (library.hasSave(game)) {
                        confirmSaveImport = game
                    } else {
                        saveTarget = game
                        saveImporter.launch(arrayOf("*/*"))
                    }
                }
                if (library.hasSave(game)) {
                    SheetAction(R.drawable.ic_upload, "세이브 내보내기", null) {
                        close()
                        saveTarget = game
                        saveExporter.launch("${game.baseName}.sav")
                    }
                }
                SheetAction(R.drawable.ic_delete, "라이브러리에서 삭제", null, danger = true) {
                    close()
                    deleteGame = game
                }
            }
        }
    }

    infoGame?.let { game ->
        GameInfoSheet(
            game = game,
            library = library,
            onDismiss = { infoGame = null },
            onRename = { alias ->
                library.setAlias(game, alias)
                infoGame = null
                refresh++
            },
        )
    }

    confirmSaveImport?.let { game ->
        AlertDialog(
            onDismissRequest = { confirmSaveImport = null },
            icon = { Icon(painterResource(R.drawable.ic_download), null) },
            title = { Text("세이브를 덮어쓸까요?") },
            text = {
                Text("'${game.title}'의 현재 세이브를 고른 파일로 바꿉니다. 지금 세이브는 백업 파일(.sav.bak)로 보관됩니다.\n\n" +
                    "실기 카트리지에서 추출한 .sav, 다른 에뮬레이터의 .sav/.srm, DeSmuME의 .dsv를 쓸 수 있습니다.")
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmSaveImport = null
                    saveTarget = game
                    saveImporter.launch(arrayOf("*/*"))
                }) { Text("파일 고르기") }
            },
            dismissButton = { TextButton(onClick = { confirmSaveImport = null }) { Text("취소") } },
        )
    }

    if (confirmRestore) {
        AlertDialog(
            onDismissRequest = { confirmRestore = false },
            icon = { Icon(painterResource(R.drawable.ic_restore), null) },
            title = { Text("백업에서 복원할까요?") },
            text = { Text("백업 ZIP에 들어 있는 세이브와 상태 저장 파일로, 같은 이름의 지금 파일을 덮어씁니다.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmRestore = false
                    backupReader.launch(arrayOf("application/zip", "application/octet-stream", "*/*"))
                }) { Text("파일 고르기") }
            },
            dismissButton = { TextButton(onClick = { confirmRestore = false }) { Text("취소") } },
        )
    }

    slot2For?.let { nds ->
        val gbaGames = games.filter { it.platform == Platform.GBA }
        val current = remember(nds) { library.slot2Game(nds) }
        ModalBottomSheet(onDismissRequest = { slot2For = null }) {
            Column(Modifier.navigationBarsPadding().padding(bottom = 16.dp)) {
                Text("GBA 슬롯", style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(horizontal = 24.dp))
                Text(
                    "DS 본체의 GBA 슬롯에 꽂을 게임을 고르세요. 이 GBA 게임의 세이브 파일을 그대로 사용하므로, " +
                        "팔파크 등으로 옮긴 결과가 GBA 쪽 세이브에도 반영됩니다.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp),
                )
                if (gbaGames.isEmpty()) {
                    Text("라이브러리에 GBA 게임이 없습니다.", style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.padding(24.dp))
                }
                gbaGames.forEach { gba ->
                    ListItem(
                        headlineContent = { Text(gba.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        supportingContent = if (gba.id == current?.id) ({ Text("꽂혀 있음", color = MaterialTheme.colorScheme.primary) }) else null,
                        leadingContent = { GameIcon(gba) },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        modifier = Modifier.combinedClickable(onClick = {
                            library.setSlot2Game(nds, gba)
                            slot2For = null
                            refresh++
                        }),
                    )
                }
                if (current != null) {
                    SheetAction(R.drawable.ic_close, "카트리지 빼기", null, danger = true) {
                        library.setSlot2Game(nds, null)
                        slot2For = null
                        refresh++
                    }
                }
            }
        }
    }

    deleteGame?.let { game ->
        AlertDialog(
            onDismissRequest = { deleteGame = null },
            icon = { Icon(painterResource(R.drawable.ic_delete), null) },
            title = { Text("삭제할까요?") },
            text = { Text("'${game.title}' ROM을 라이브러리에서 삭제합니다. 세이브 파일은 남겨둡니다.") },
            confirmButton = {
                TextButton(onClick = {
                    library.delete(game)
                    deleteGame = null
                    refresh++
                }) { Text("삭제", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { deleteGame = null }) { Text("취소") } },
        )
    }
}

/** ROM details and the name shown for it (an alias; files keep their names). */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GameInfoSheet(game: Game, library: GameLibrary, onDismiss: () -> Unit, onRename: (String?) -> Unit) {
    var name by remember { mutableStateOf(if (game.hasAlias) game.title else "") }
    val details = remember(game) {
        buildList {
            addAll(RomInfo.header(game))
            addAll(RomInfo.file(game))
            val save = library.savePath(game)
            add("배터리 세이브" to if (save.exists()) RomInfo.size(save.length()) else "없음")
            val states = (1..8).count { library.statePath(game, it).exists() }
            add("상태 저장" to if (states > 0) "${states}개" else "없음")
            val cheats = library.cheats(game).load()
            if (cheats.isNotEmpty()) add("치트" to "${cheats.size}개 (${cheats.count { it.enabled }}개 켜짐)")
        }
    }
    var crc by remember(game) { mutableStateOf<String?>(null) }
    LaunchedEffect(game) {
        crc = withContext(Dispatchers.IO) { runCatching { RomInfo.crc32(game.file) }.getOrDefault("읽을 수 없음") }
    }
    val trimmed = name.trim()
    val changed = trimmed != (if (game.hasAlias) game.title else "")

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(
            Modifier.navigationBarsPadding().imePadding().verticalScroll(rememberScrollState()).padding(bottom = 16.dp),
        ) {
            ListItem(
                headlineContent = { Text(game.title, style = MaterialTheme.typography.titleLarge) },
                supportingContent = { Text(game.platform.label) },
                leadingContent = { GameIcon(game) },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
            OutlinedTextField(
                value = name,
                onValueChange = { name = it.replace('\n', ' ') },
                label = { Text("표시 이름") },
                placeholder = { Text(game.originalTitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                supportingText = { Text("비워 두면 원래 이름으로 보입니다. 세이브와 상태 저장은 그대로입니다.") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { if (changed) onRename(trimmed) }),
                modifier = Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 4.dp),
            )
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                horizontalArrangement = Arrangement.End,
            ) {
                if (game.hasAlias) {
                    TextButton(onClick = { onRename(null) }) { Text("원래 이름으로") }
                    Spacer(Modifier.width(8.dp))
                }
                FilledTonalButton(onClick = { onRename(trimmed) }, enabled = changed) { Text("저장") }
            }
            HorizontalDivider(Modifier.padding(horizontal = 24.dp, vertical = 12.dp))
            Text(
                "ROM 정보",
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 4.dp),
            )
            (details + ("CRC32" to (crc ?: "계산 중…"))).forEach { (label, value) ->
                Row(Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 6.dp)) {
                    Text(
                        label,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(112.dp),
                    )
                    Text(
                        value,
                        style = MaterialTheme.typography.bodyMedium,
                        fontFamily = if (label == "CRC32" || label == "게임 코드") FontFamily.Monospace else null,
                    )
                }
            }
        }
    }
}

@Composable
private fun SheetAction(icon: Int, title: String, detail: String?, danger: Boolean = false, onClick: () -> Unit) {
    val color = if (danger) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
    ListItem(
        headlineContent = { Text(title, color = color) },
        supportingContent = detail?.let { { Text(it, maxLines = 1, overflow = TextOverflow.Ellipsis) } },
        leadingContent = {
            Icon(painterResource(icon), null, tint = if (danger) color else MaterialTheme.colorScheme.onSurfaceVariant)
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.combinedClickable(onClick = onClick).padding(horizontal = 8.dp),
    )
}

@Composable
private fun EmptyState(filtered: Boolean, onAdd: () -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            Modifier.size(96.dp).clip(RoundedCornerShape(28.dp)).background(MaterialTheme.colorScheme.secondaryContainer),
            contentAlignment = Alignment.Center,
        ) {
            Icon(painterResource(R.drawable.ic_gamepad), null, Modifier.size(48.dp), tint = MaterialTheme.colorScheme.onSecondaryContainer)
        }
        Spacer(Modifier.height(24.dp))
        Text(if (filtered) "이 기종의 게임이 없습니다" else "게임이 없습니다", style = MaterialTheme.typography.headlineSmall)
        Spacer(Modifier.height(8.dp))
        Text(
            "직접 소유한 카트리지에서 추출한 ROM 파일(.gb .gbc .gba .nds)을 추가하세요. .zip으로 묶인 파일도 됩니다.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(24.dp))
        FilledTonalButton(onClick = onAdd) {
            Icon(painterResource(R.drawable.ic_add), null, Modifier.size(18.dp))
            Spacer(Modifier.size(8.dp))
            Text("ROM 추가")
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameRow(game: Game, onClick: () -> Unit, onLongClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(game.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = {
            Column {
                val detail = buildString {
                    append(game.platform.label)
                    game.subtitle?.let { append(" · ").append(it) }
                }
                Text(detail, maxLines = 1, overflow = TextOverflow.Ellipsis)
                game.slot2Title?.let { slot2 ->
                    Text("GBA 슬롯: $slot2", style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        },
        leadingContent = { GameIcon(game) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .padding(horizontal = 8.dp)
            .clip(RoundedCornerShape(16.dp))
            .combinedClickable(onClick = onClick, onLongClick = onLongClick),
    )
}

@Composable
private fun GameIcon(game: Game) {
    val shape = RoundedCornerShape(14.dp)
    val icon = game.icon
    val (container, content) = platformColors(game.platform)
    Box(Modifier.size(56.dp).clip(shape).background(container), contentAlignment = Alignment.Center) {
        if (icon != null) {
            Image(
                bitmap = remember(icon) { icon.asImageBitmap() },
                contentDescription = null,
                filterQuality = FilterQuality.None,
                modifier = Modifier.size(44.dp),
            )
        } else {
            Text(game.platform.shortLabel, color = content, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleSmall)
        }
    }
}
