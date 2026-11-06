package com.emulate.app

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

enum class Platform(val label: String, val shortLabel: String, val corePath: String, val extensions: Set<String>) {
    GB("Game Boy", "GB", "libmgba_libretro.so", setOf("gb")),
    GBC("Game Boy Color", "GBC", "libmgba_libretro.so", setOf("gbc")),
    GBA("Game Boy Advance", "GBA", "libmgba_libretro.so", setOf("gba")),
    NDS("Nintendo DS", "NDS", "libmelondsds_libretro.so", setOf("nds"));

    /** Core options we override per platform (everything else uses the core's defaults). */
    val coreOptions: Map<String, String>
        get() = when (this) {
            GB, GBC, GBA -> mapOf(
                "mgba_sgb_borders" to "OFF",
                "mgba_skip_bios" to "ON",
            )
            NDS -> mapOf(
                "melonds_touch_mode" to "touch",
                "melonds_show_cursor" to "disabled",
                "melonds_screen_layout1" to "top-bottom",
                "melonds_number_of_screen_layouts" to "1",
                "melonds_show_current_layout" to "disabled",
                // No host microphone yet: the MIC button feeds a simulated "blow" while held.
                "melonds_mic_input" to "blow",
                "melonds_mic_input_active" to "hold",
            )
        }

    companion object {
        fun fromExtension(ext: String): Platform? =
            entries.firstOrNull { ext.lowercase(Locale.ROOT) in it.extensions }
    }
}

data class Game(
    val file: File,
    val platform: Platform,
    /** What the app shows: the user's alias if any, else [originalTitle]. */
    val title: String,
    val subtitle: String?,
    val icon: Bitmap?,
    /** The NDS banner title, else the file name. */
    val originalTitle: String = title,
) {
    val id: String get() = file.name
    val baseName: String get() = file.nameWithoutExtension
    val hasAlias: Boolean get() = title != originalTitle
}

/**
 * ROMs are imported (copied) into app storage so they stay available regardless of where they
 * were picked from. Battery saves for a ROM live at saves/<rom name>.sav — the NDS core's GBA
 * slot uses that same file, which is what makes Pal Park-style transfers work.
 */
class GameLibrary(private val context: Context) {
    val romDir = File(context.filesDir, "roms").apply { mkdirs() }
    val saveDir = File(context.filesDir, "saves").apply { mkdirs() }
    val stateDir = File(context.filesDir, "states").apply { mkdirs() }
    val systemDir = File(context.filesDir, "system").apply { mkdirs() }

    private val prefs = context.getSharedPreferences("library", Context.MODE_PRIVATE)

    fun games(): List<Game> {
        val roms = romDir.listFiles().orEmpty().filter { Platform.fromExtension(it.extension) != null }
        migrateLegacySaves(roms)
        return roms.map { load(it, Platform.fromExtension(it.extension)!!) }
            .sortedWith(compareBy({ it.platform.ordinal }, { it.title.lowercase() }))
    }

    fun find(id: String): Game? = File(romDir, id).takeIf { it.exists() }?.let { file ->
        Platform.fromExtension(file.extension)?.let { load(file, it) }
    }

    private fun load(file: File, platform: Platform): Game {
        val banner = if (platform == Platform.NDS) runCatching { NdsBanner.read(file) }.getOrNull() else null
        val original = banner?.title ?: file.nameWithoutExtension
        val alias = prefs.getString(aliasKey(file.name), null)
        return Game(file, platform, alias ?: original, banner?.subtitle, banner?.icon, originalTitle = original)
    }

    // ---- aliases ----
    // Only the shown name changes; saves and states stay keyed by the ROM's file name.

    /** Sets the name shown for [game]; blank (or the original title) goes back to the original. */
    fun setAlias(game: Game, alias: String?) {
        val name = alias?.trim()?.takeIf { it.isNotEmpty() && it != game.originalTitle }
        prefs.edit().apply {
            if (name == null) remove(aliasKey(game.id)) else putString(aliasKey(game.id), name)
        }.apply()
    }

    private fun aliasKey(id: String) = "alias:$id"

    /**
     * Copies a picked document into the library. ZIP archives are unpacked (every supported ROM
     * inside is added). Returns the games added; empty if the file isn't a supported ROM.
     */
    fun import(uri: Uri): List<Game> {
        val name = sanitize(displayName(uri) ?: return emptyList())
        val resolver = context.contentResolver
        if (name.endsWith(".zip", ignoreCase = true)) {
            val games = ArrayList<Game>()
            resolver.openInputStream(uri)?.use { raw ->
                ZipInputStream(raw.buffered()).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        if (entry.isDirectory) continue
                        val entryName = sanitize(entry.name.substringAfterLast('/'))
                        if (Platform.fromExtension(entryName.substringAfterLast('.', "")) == null) continue
                        storeRom(entryName, zip)?.let { games += it }
                    }
                }
            }
            return games
        }
        if (Platform.fromExtension(name.substringAfterLast('.', "")) == null) return emptyList()
        return resolver.openInputStream(uri)?.use { input -> listOfNotNull(storeRom(name, input)) } ?: emptyList()
    }

    private fun storeRom(name: String, input: InputStream): Game? {
        val platform = Platform.fromExtension(name.substringAfterLast('.', "")) ?: return null
        val target = File(romDir, name)
        val tmp = File(romDir, "$name.part")
        tmp.outputStream().use { output -> input.copyTo(output, 1 shl 16) }
        if (!tmp.renameTo(target)) {
            tmp.delete()
            return null
        }
        return load(target, platform)
    }

    private fun displayName(uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        } ?: uri.lastPathSegment

    private fun sanitize(name: String) = name.substringAfterLast('/').replace(Regex("[\\\\:*?\"<>|]"), "_")

    // ---- battery saves ----

    fun hasSave(game: Game) = savePath(game).exists()

    /**
     * Replaces the game's battery save with a picked file (.sav/.srm, or a DeSmuME .dsv whose
     * footer is stripped). The previous save is kept as <rom file>.sav.bak. Returns an error or null.
     */
    fun importSave(game: Game, uri: Uri): String? {
        val bytes = context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
            ?: return "파일을 읽을 수 없습니다."
        if (bytes.isEmpty()) return "빈 파일입니다."
        if (bytes.size > 32 * 1024 * 1024) return "세이브 파일이 너무 큽니다."
        val data = stripDesmumeFooter(bytes)
        val target = savePath(game)
        if (target.exists()) target.copyTo(File(saveDir, "${game.id}.sav.bak"), overwrite = true)
        val tmp = File(saveDir, "${game.id}.sav.tmp")
        tmp.writeBytes(data)
        if (!tmp.renameTo(target)) {
            tmp.delete()
            return "세이브를 저장하지 못했습니다."
        }
        return null
    }

    // ---- optional DS BIOS / firmware dumps ----
    // melonDS DS looks for these in system/melonDS DS/. With all three present it runs the real
    // BIOS and firmware; otherwise it uses its built-in replacements (fine for nearly all games).

    private val ndsSystemDir get() = File(systemDir, "melonDS DS")

    /** File name -> present, for bios7.bin, bios9.bin and firmware.bin. */
    fun ndsSystemFiles(): Map<String, Boolean> =
        NDS_SYSTEM_FILES.associateWith { File(ndsSystemDir, it).exists() }

    /** Recognises a dump by its size and stores it. Returns the stored file name, or null if unknown. */
    fun importNdsSystemFile(uri: Uri): String? {
        val bytes = context.contentResolver.openInputStream(uri)?.use { input ->
            val buffer = ByteArray(512 * 1024 + 1)
            var total = 0
            while (total < buffer.size) {
                val n = input.read(buffer, total, buffer.size - total)
                if (n < 0) break
                total += n
            }
            buffer.copyOf(total)
        } ?: return null
        val name = when (bytes.size) {
            16 * 1024 -> "bios7.bin"
            4 * 1024 -> "bios9.bin"
            128 * 1024, 256 * 1024, 512 * 1024 -> "firmware.bin"
            else -> return null
        }
        ndsSystemDir.mkdirs()
        val tmp = File(ndsSystemDir, "$name.tmp")
        tmp.writeBytes(bytes)
        if (!tmp.renameTo(File(ndsSystemDir, name))) {
            tmp.delete()
            return null
        }
        return name
    }

    fun deleteNdsSystemFiles() = NDS_SYSTEM_FILES.forEach { File(ndsSystemDir, it).delete() }

    fun exportSave(game: Game, uri: Uri): String? {
        val source = savePath(game)
        if (!source.exists()) return "아직 세이브 파일이 없습니다."
        context.contentResolver.openOutputStream(uri, "wt")?.use { out -> source.inputStream().use { it.copyTo(out) } }
            ?: return "파일을 쓸 수 없습니다."
        return null
    }

    /** DeSmuME appends a 122-byte footer ending in "|-DESMUME SAVE-|" to raw NDS saves. */
    private fun stripDesmumeFooter(bytes: ByteArray): ByteArray {
        val marker = "|-DESMUME SAVE-|".toByteArray(Charsets.US_ASCII)
        if (bytes.size <= 122 || !bytes.copyOfRange(bytes.size - marker.size, bytes.size).contentEquals(marker)) return bytes
        return bytes.copyOf(bytes.size - 122)
    }

    /** Writes every battery save, RTC file and save state into a ZIP. Returns the number of files. */
    fun backupAll(uri: Uri): Int {
        var count = 0
        context.contentResolver.openOutputStream(uri, "wt")?.use { raw ->
            ZipOutputStream(raw.buffered()).use { zip ->
                for ((dir, prefix) in listOf(saveDir to "saves/", stateDir to "states/")) {
                    dir.listFiles().orEmpty().filter { it.isFile && !it.name.endsWith(".tmp") }.forEach { file ->
                        zip.putNextEntry(ZipEntry(prefix + file.name))
                        file.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                        count++
                    }
                }
            }
        }
        return count
    }

    /** Restores a ZIP made by [backupAll]; existing files with the same names are replaced. */
    fun restoreBackup(uri: Uri): Int {
        var count = 0
        context.contentResolver.openInputStream(uri)?.use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val dir = when {
                        entry.name.startsWith("saves/") -> saveDir
                        entry.name.startsWith("states/") -> stateDir
                        else -> continue
                    }
                    val name = sanitize(entry.name.substringAfterLast('/'))
                    if (entry.isDirectory || name.isEmpty()) continue
                    File(dir, name).outputStream().use { zip.copyTo(it) }
                    count++
                }
            }
        }
        return count
    }

    fun delete(game: Game) {
        game.file.delete()
        prefs.edit().remove(slot2Key(game)).remove(aliasKey(game.id)).apply()
    }

    // Keyed by the full file name: "Game.gba" and "Game.nds" must not share a save.
    fun savePath(game: Game) = File(saveDir, "${game.id}.sav")
    fun rtcPath(game: Game) = File(saveDir, "${game.id}.rtc")
    fun statePath(game: Game, slot: Int) = File(stateDir, "${game.id}.state$slot")
    /** Screenshot taken when the state was saved. */
    fun stateThumbnailPath(game: Game, slot: Int) = File(stateDir, "${game.id}.state$slot.png")

    fun deleteState(game: Game, slot: Int) {
        statePath(game, slot).delete()
        stateThumbnailPath(game, slot).delete()
    }
    fun cheats(game: Game) = CheatStore(File(context.filesDir, "cheats/${game.id}.json"))

    /** Earlier versions named saves after the ROM name without its extension; move them over. */
    private fun migrateLegacySaves(roms: List<File>) {
        for ((base, sharing) in roms.groupBy { it.nameWithoutExtension }) {
            val legacy = buildList {
                add(saveDir to ".sav"); add(saveDir to ".rtc"); add(saveDir to ".sav.bak")
                for (slot in 1..4) add(stateDir to ".state$slot")
            }
            for ((dir, suffix) in legacy) {
                val old = File(dir, base + suffix)
                if (!old.isFile) continue
                // If several ROMs shared the old name they shared this file too, so each keeps a copy.
                for (rom in sharing) {
                    val new = File(dir, rom.name + suffix)
                    if (!new.exists()) old.copyTo(new)
                }
                old.delete()
            }
        }
    }

    companion object {
        val NDS_SYSTEM_FILES = listOf("bios7.bin", "bios9.bin", "firmware.bin")
    }

    /** GBA game "inserted" into this NDS game's Slot-2, if any. */
    fun slot2Game(nds: Game): Game? =
        prefs.getString(slot2Key(nds), null)?.let { find(it) }?.takeIf { it.platform == Platform.GBA }

    fun setSlot2Game(nds: Game, gba: Game?) {
        prefs.edit().apply {
            if (gba == null) remove(slot2Key(nds)) else putString(slot2Key(nds), gba.id)
        }.apply()
    }

    private fun slot2Key(game: Game) = "slot2:${game.id}"
}

/** Reads the title and 32x32 icon from an NDS ROM's banner. */
object NdsBanner {
    class Banner(val title: String, val subtitle: String?, val icon: Bitmap?)

    fun read(file: File): Banner? = RandomAccessFile(file, "r").use { raf ->
        val header = ByteArray(0x70)
        raf.readFully(header)
        val bannerOffset = u32(header, 0x68)
        if (bannerOffset == 0L || bannerOffset + 0x840 > raf.length()) return null
        raf.seek(bannerOffset)
        val banner = ByteArray(0xA40)
        val read = raf.read(banner)
        if (read < 0x840) return null
        val version = (banner[0].toInt() and 0xFF) or ((banner[1].toInt() and 0xFF) shl 8)

        val lang = Locale.getDefault().language
        val titleOffset = when {
            lang == "ko" && version >= 3 && read >= 0xA40 -> 0x940
            lang == "ja" -> 0x240
            lang == "zh" && version >= 2 && read >= 0x940 -> 0x840
            else -> 0x340  // English
        }
        val text = String(banner, titleOffset, 0x100, Charsets.UTF_16LE).substringBefore('\u0000')
        val lines = text.lines().map { it.trim() }.filter { it.isNotEmpty() }
        val title = lines.firstOrNull() ?: return null
        // 3-line titles are "title / subtitle / publisher"; 2-line ones are "title / publisher".
        val subtitle = if (lines.size >= 3) lines[1] else null
        Banner(title, subtitle, decodeIcon(banner))
    }

    private fun decodeIcon(banner: ByteArray): Bitmap {
        val palette = IntArray(16) { i ->
            val c = (banner[0x220 + i * 2].toInt() and 0xFF) or ((banner[0x221 + i * 2].toInt() and 0xFF) shl 8)
            val r = (c and 0x1F) * 255 / 31
            val g = ((c shr 5) and 0x1F) * 255 / 31
            val b = ((c shr 10) and 0x1F) * 255 / 31
            if (i == 0) 0 else (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        val pixels = IntArray(32 * 32)
        // 4x4 tiles of 8x8 pixels, 4 bits per pixel, low nibble first.
        for (tile in 0 until 16) {
            val tx = (tile % 4) * 8
            val ty = (tile / 4) * 8
            for (i in 0 until 32) {
                val byte = banner[0x20 + tile * 32 + i].toInt() and 0xFF
                val px = (i % 4) * 2
                val py = i / 4
                pixels[(ty + py) * 32 + tx + px] = palette[byte and 0xF]
                pixels[(ty + py) * 32 + tx + px + 1] = palette[byte shr 4]
            }
        }
        return Bitmap.createBitmap(pixels, 32, 32, Bitmap.Config.ARGB_8888)
    }

    private fun u32(b: ByteArray, o: Int): Long =
        ((b[o].toLong() and 0xFF) or ((b[o + 1].toLong() and 0xFF) shl 8) or
            ((b[o + 2].toLong() and 0xFF) shl 16) or ((b[o + 3].toLong() and 0xFF) shl 24))
}
