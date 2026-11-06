package com.emulate.app

import android.content.Context
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import kotlin.random.Random

/** Kotlin side of the NDS local wireless session (see app/src/main/cpp/netplay.h). */
object Netplay {
    const val PORT = 47000

    enum class State { IDLE, HOSTING, JOINING, CLIENT }

    data class HostInfo(val address: String, val title: String, val players: Int, val maxPlayers: Int, val tag: String)

    /** Consoles that can talk to each other share a tag (GB and GBC link cables are compatible). */
    fun tag(platform: Platform) = when (platform) {
        Platform.GB, Platform.GBC -> "GB"
        Platform.GBA -> "GBA"
        Platform.NDS -> "NDS"
    }

    data class LocalAddress(val address: Inet4Address, val broadcast: InetAddress?, val interfaceName: String)

    fun state(value: Int) = State.entries.getOrElse(value) { State.IDLE }

    /** Site-local IPv4 addresses of this device (Wi-Fi, hotspot, ...), Wi-Fi interfaces first. */
    fun localAddresses(): List<LocalAddress> =
        runCatching { NetworkInterface.getNetworkInterfaces().toList() }.getOrDefault(emptyList())
            .filter { runCatching { it.isUp && !it.isLoopback }.getOrDefault(false) }
            .flatMap { nif ->
                nif.interfaceAddresses.mapNotNull { ia ->
                    val addr = ia.address as? Inet4Address ?: return@mapNotNull null
                    if (!addr.isSiteLocalAddress) return@mapNotNull null
                    LocalAddress(addr, ia.broadcast, nif.name)
                }
            }
            .sortedBy { if (it.interfaceName.startsWith("wlan") || it.interfaceName.contains("ap")) 0 else 1 }

    /** Broadcasts a discovery request on every local network and collects host announcements. */
    fun scan(timeoutMs: Int = 1500): List<HostInfo> {
        val request = byteArrayOf('E'.code.toByte(), 'M'.code.toByte(), 'N'.code.toByte(), 'P'.code.toByte(),
            1, 1, 0, 0, 0, 0)  // version 1, DISCOVER, src 0, dst 0
        val targets = (localAddresses().mapNotNull { it.broadcast } + InetAddress.getByName("255.255.255.255"))
            .distinct()
        val found = LinkedHashMap<String, HostInfo>()
        DatagramSocket().use { socket ->
            socket.broadcast = true
            socket.soTimeout = 200
            val deadline = System.currentTimeMillis() + timeoutMs
            var nextSend = 0L
            val buffer = ByteArray(1500)
            while (System.currentTimeMillis() < deadline) {
                if (System.currentTimeMillis() >= nextSend) {
                    for (target in targets) runCatching { socket.send(DatagramPacket(request, request.size, target, PORT)) }
                    nextSend = System.currentTimeMillis() + 500
                }
                val packet = DatagramPacket(buffer, buffer.size)
                try {
                    socket.receive(packet)
                } catch (_: SocketTimeoutException) {
                    continue
                }
                val data = packet.data
                val len = packet.length
                if (len < 12 || String(data, 0, 4, Charsets.US_ASCII) != "EMNP" || data[5].toInt() != 2) continue
                val address = packet.address.hostAddress ?: continue
                val fields = String(data, 12, len - 12, Charsets.UTF_8).split('\u0000')
                found[address] = HostInfo(address, fields[0], data[10].toInt() and 0xFF, data[11].toInt() and 0xFF,
                    fields.getOrElse(1) { "" })
            }
        }
        return found.values.toList()
    }

    /**
     * A per-install DS MAC address. The built-in firmware's MAC is the same on every device, which
     * would make all consoles look identical to each other over local wireless. It's generated
     * once and kept forever, because some games tie their save data to the console's MAC.
     */
    fun macAddress(context: Context): String {
        val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        prefs.getString("ds_mac", null)?.let { return it }
        // 00:08:BF is melonDS DS's "not the default firmware" prefix.
        val mac = "00:08:BF:" + (0 until 3).joinToString(":") { "%02X".format(Random.nextInt(256)) }
        prefs.edit().putString("ds_mac", mac).apply()
        return mac
    }
}
