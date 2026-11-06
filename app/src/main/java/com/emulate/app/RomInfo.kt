package com.emulate.app

import java.io.File
import java.io.RandomAccessFile
import java.text.DateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.CRC32

/** What the info screen shows about a ROM: label -> value rows, read from its header. */
object RomInfo {

    fun header(game: Game): List<Pair<String, String>> = runCatching {
        when (game.platform) {
            Platform.GB, Platform.GBC -> gb(game.file)
            Platform.GBA -> gba(game.file)
            Platform.NDS -> nds(game.file)
        }
    }.getOrDefault(listOf("헤더" to "읽을 수 없습니다"))

    /** File name, size and date added. */
    fun file(game: Game): List<Pair<String, String>> = listOf(
        "파일 이름" to game.file.name,
        "크기" to size(game.file.length()),
        "추가한 날짜" to DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(game.file.lastModified())),
    )

    /** CRC32 of the whole file (what No-Intro / ROM databases list), as 8 hex digits. Slow for big ROMs. */
    fun crc32(file: File): String {
        val crc = CRC32()
        val buffer = ByteArray(1 shl 16)
        file.inputStream().use { input ->
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                crc.update(buffer, 0, n)
            }
        }
        return String.format(Locale.ROOT, "%08X", crc.value)
    }

    fun size(bytes: Long): String = when {
        bytes >= 1 shl 20 -> String.format(Locale.ROOT, "%.1f MB", bytes / 1048576.0).replace(".0 MB", " MB")
        bytes >= 1 shl 10 -> "${bytes / 1024} KB"
        else -> "$bytes B"
    }

    // ---- headers ----

    private fun read(file: File, length: Int): ByteArray = RandomAccessFile(file, "r").use { raf ->
        val bytes = ByteArray(minOf(length.toLong(), raf.length()).toInt())
        raf.readFully(bytes)
        bytes
    }

    private fun ByteArray.u8(offset: Int) = this[offset].toInt() and 0xFF

    /** Printable ASCII up to the first NUL / non-printable byte. */
    private fun ByteArray.ascii(offset: Int, length: Int): String =
        (offset until offset + length).map { u8(it) }.takeWhile { it in 0x20..0x7E }
            .joinToString("") { it.toChar().toString() }.trim()

    private fun gb(file: File): List<Pair<String, String>> {
        val b = read(file, 0x150)
        if (b.size < 0x150) return listOf("헤더" to "파일이 너무 짧습니다")
        val cgb = b.u8(0x143)
        val title = b.ascii(0x134, if (cgb and 0x80 != 0) 15 else 16)
        var checksum = 0
        for (i in 0x134..0x14C) checksum = checksum - b.u8(i) - 1
        val licensee = if (b.u8(0x14B) == 0x33) b.ascii(0x144, 2) else String.format(Locale.ROOT, "%02X", b.u8(0x14B))
        val ramKb = when (b.u8(0x149)) { 2 -> 8; 3 -> 32; 4 -> 128; 5 -> 64; else -> 0 }
        return listOfNotNull(
            "내부 제목" to title.ifEmpty { "(없음)" },
            "대상 기기" to when {
                cgb == 0xC0 -> "게임보이 컬러 전용"
                cgb and 0x80 != 0 -> "게임보이 컬러 대응 (흑백 게임보이에서도 실행)"
                else -> "게임보이"
            } + if (b.u8(0x146) == 0x03) " · 슈퍼 게임보이 대응" else "",
            "카트리지" to gbCartridge(b.u8(0x147)),
            "ROM 크기" to (if (b.u8(0x148) <= 8) size(32768L shl b.u8(0x148)) else "알 수 없음"),
            "세이브 RAM" to if (ramKb > 0) "$ramKb KB" else if (b.u8(0x147) in setOf(0x05, 0x06)) "512 × 4비트 (MBC2 내장)" else "없음",
            "지역" to if (b.u8(0x14A) == 0) "일본" else "해외",
            "제조사" to maker(licensee),
            "개정판" to "Rev ${b.u8(0x14C)}",
            "헤더 체크섬" to if (checksum and 0xFF == b.u8(0x14D)) "정상" else "불일치 (변형된 ROM일 수 있음)",
        )
    }

    private fun gbCartridge(type: Int): String {
        val name = when (type) {
            0x00 -> "ROM만"
            0x01, 0x02, 0x03 -> "MBC1"
            0x05, 0x06 -> "MBC2"
            0x0B, 0x0C, 0x0D -> "MMM01"
            0x0F, 0x10, 0x11, 0x12, 0x13 -> "MBC3"
            0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E -> "MBC5"
            0x20 -> "MBC6"
            0x22 -> "MBC7 (기울기 센서)"
            0xFC -> "포켓 카메라"
            0xFD -> "반다이 TAMA5"
            0xFE -> "HuC3"
            0xFF -> "HuC1 (적외선)"
            else -> String.format(Locale.ROOT, "알 수 없음 (0x%02X)", type)
        }
        val extras = buildList {
            if (type in setOf(0x03, 0x06, 0x0D, 0x0F, 0x10, 0x13, 0x1B, 0x1E, 0x22, 0xFF)) add("배터리 세이브")
            if (type in setOf(0x0F, 0x10)) add("시계(RTC)")
            if (type in 0x1C..0x1E) add("진동")
        }
        return if (extras.isEmpty()) name else "$name · ${extras.joinToString(" · ")}"
    }

    private fun gba(file: File): List<Pair<String, String>> {
        val b = read(file, 0xC0)
        if (b.size < 0xC0) return listOf("헤더" to "파일이 너무 짧습니다")
        val code = b.ascii(0xAC, 4)
        var checksum = 0
        for (i in 0xA0..0xBC) checksum -= b.u8(i)
        checksum = (checksum - 0x19) and 0xFF
        return listOf(
            "내부 제목" to b.ascii(0xA0, 12).ifEmpty { "(없음)" },
            "게임 코드" to (if (code.length == 4) "AGB-$code" else "(없음)"),
            "지역" to region(code),
            "제조사" to maker(b.ascii(0xB0, 2)),
            "개정판" to "Rev ${b.u8(0xBC)}",
            "헤더 체크섬" to if (checksum == b.u8(0xBD)) "정상" else "불일치 (변형된 ROM일 수 있음)",
        )
    }

    private fun nds(file: File): List<Pair<String, String>> {
        val b = read(file, 0x20)
        if (b.size < 0x20) return listOf("헤더" to "파일이 너무 짧습니다")
        val code = b.ascii(0x0C, 4)
        return listOf(
            "내부 제목" to b.ascii(0x00, 12).ifEmpty { "(없음)" },
            "게임 코드" to (if (code.length == 4) "NTR-$code" else "(없음)"),
            "지역" to region(code),
            "제조사" to maker(b.ascii(0x10, 2)),
            "대상 기기" to when (b.u8(0x12)) {
                0x02 -> "DS (DSi 기능 일부 대응)"
                0x03 -> "DSi 전용"
                else -> "DS"
            },
            "카트리지 용량" to if (b.u8(0x14) <= 15) size(131072L shl b.u8(0x14)) else "알 수 없음",
            "개정판" to "Rev ${b.u8(0x1E)}",
        )
    }

    /** Last letter of a GBA/NDS game code. */
    private fun region(code: String): String = when (code.lastOrNull()) {
        'J' -> "일본"
        'E' -> "북미"
        'P' -> "유럽"
        'K' -> "한국"
        'D' -> "독일"
        'F' -> "프랑스"
        'S' -> "스페인"
        'I' -> "이탈리아"
        'H' -> "네덜란드"
        'U' -> "호주"
        'C' -> "중국"
        'O' -> "국제판"
        null -> "알 수 없음"
        else -> "기타 (${code.last()})"
    }

    private fun maker(code: String): String {
        val name = when (code) {
            "01" -> "닌텐도"
            "08" -> "캡콤"
            "13", "69" -> "EA"
            "18" -> "허드슨"
            "20" -> "KSS"
            "41" -> "유비소프트"
            "4F" -> "아이도스"
            "51" -> "어클레임"
            "52" -> "액티비전"
            "5G" -> "마제스코"
            "64" -> "루카스아츠"
            "70" -> "인포그램즈"
            "78" -> "THQ"
            "7D" -> "비벤디"
            "8P" -> "세가"
            "99" -> "아크 시스템 웍스"
            "A4" -> "코나미"
            "AF" -> "남코"
            "B0" -> "어클레임 재팬"
            "B1" -> "아스키"
            "B2" -> "반다이"
            "C8" -> "코에이"
            "E7" -> "아테나"
            "E9" -> "나츠메"
            "EB" -> "아틀러스"
            "G9" -> "D3 퍼블리셔"
            "GD" -> "스퀘어 에닉스"
            "HF" -> "레벨 파이브"
            "LT" -> "레거시 인터랙티브"
            else -> null
        }
        return when {
            code.isEmpty() || code == "00" -> "알 수 없음"
            name != null -> "$name ($code)"
            else -> code
        }
    }
}
