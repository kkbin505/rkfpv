package io.github.kkbin505.rkfpv.board

import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.security.SecureRandom

/**
 * The board's fpvd (src/fpvd.c): `fpv` commands over UDP 7000, no shell.
 * Board images whose hotspot has no /api/fpv yet still run it, so settings
 * work over their hotspot too.
 *
 *   request:  FPV1 <id> <cmd> [arg ...]
 *   reply:    FPV1 <id> <seq>/<total>\n<chunk>    (one or more datagrams)
 *
 * Lost datagrams: resend the request with the SAME id from the same socket
 * (fpvd caches replies by id and sender, so a resent `set` or `restart` is
 * not run twice). Same as the PC configurator's Fpvd transport.
 */
class FpvdClient(
    private val host: String,
    private val port: Int = PORT,
    /** on the phone: a socket bound to the hotspot's Network */
    private val socket: () -> DatagramSocket = { DatagramSocket() },
    private val tries: Int = 4,
    /** one try covers a slow `fpv schema` on the board */
    private val tryTimeoutMs: Int = 4000,
) {
    /** One `fpv <args> --json`; returns the JSON text. */
    fun run(args: List<String>): String {
        val id = newId()
        val req = ("$MAGIC $id " + args.joinToString(" ")).toByteArray(Charsets.US_ASCII)
        val parts = HashMap<Int, ByteArray>()
        val buf = ByteArray(65535)
        socket().use { s ->
            // connected: an ICMP "port unreachable" comes back as an error instead of a timeout
            s.connect(InetSocketAddress(host, port))
            repeat(tries) {
                s.send(DatagramPacket(req, req.size))
                val deadline = System.currentTimeMillis() + tryTimeoutMs
                while (true) {
                    val left = deadline - System.currentTimeMillis()
                    if (left <= 0) break
                    s.soTimeout = left.toInt().coerceAtLeast(50)
                    val p = DatagramPacket(buf, buf.size)
                    try {
                        s.receive(p)
                    } catch (e: SocketTimeoutException) {
                        break
                    }
                    val nl = (0 until p.length).firstOrNull { buf[it] == '\n'.code.toByte() } ?: continue
                    val head = String(buf, 0, nl, Charsets.US_ASCII).split(" ")
                    if (head.size != 3 || head[0] != MAGIC || head[1] != id) continue
                    val (seq, total) = head[2].split("/").mapNotNull { it.toIntOrNull() }.takeIf { it.size == 2 } ?: continue
                    if (seq < 1 || seq > total) continue
                    parts[seq] = buf.copyOfRange(nl + 1, p.length)
                    if (parts.size == total) {
                        return (1..total).joinToString("") { String(parts[it]!!, Charsets.UTF_8) }
                    }
                }
            }
        }
        throw IOException("no answer from fpvd at $host:$port")
    }

    companion object {
        const val PORT = 7000
        private const val MAGIC = "FPV1"
        private val rnd = SecureRandom()

        /** fpvd ids: 1-16 of [A-Za-z0-9_-]; new per request */
        private fun newId(): String = (1..12).map { "0123456789abcdef"[rnd.nextInt(16)] }.joinToString("")

        /** fpvd's commands (ALLOWED in src/fpvd.c); preview / hotspot / iq need /api/fpv */
        val COMMANDS = setOf("get", "set", "schema", "status", "log", "restart", "mode", "reset", "dvr")
    }
}
