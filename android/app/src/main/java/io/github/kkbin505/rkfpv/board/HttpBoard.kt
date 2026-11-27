package io.github.kkbin505.rkfpv.board

import io.github.kkbin505.rkfpv.fpv.FpvException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramSocket
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * The board over its WiFi hotspot (hotspot mode): dvrhttpd on the board
 * (src/dvrhttpd.c), the same API the hosted web configurator uses.
 *
 *   POST /api/fpv     body "set bitrate 4000": one `fpv ... --json`
 *   GET  /live        rkfpv's preview stream (after `fpv preview on`)
 *   GET  /dvr/<name>  a recording
 *
 * Changes need the header X-FPV-Confirm (dvrhttpd's guard against other
 * web pages); an app is no web page, so it simply sends it.
 *
 * Older board images: their dvrhttpd serves the recordings only (404 on
 * /api/fpv). Then `fpv` commands go to fpvd on UDP 7000 instead
 * ([FpvdClient]), which those images run as well; only the preview (and
 * hotspot / iq) needs the newer image.
 *
 * [open] and [datagramSocket] make the connections: on the phone they are
 * bound to the hotspot's android.net.Network, so requests go over WiFi even
 * though the hotspot has no internet and Android keeps mobile data as the
 * default route.
 */
class HttpBoard(
    host: String,
    private val open: (URL) -> HttpURLConnection = { it.openConnection() as HttpURLConnection },
    datagramSocket: () -> DatagramSocket = { DatagramSocket() },
    /** false once the WiFi network is gone */
    private val networkUp: () -> Boolean = { true },
    fpvdPort: Int = FpvdClient.PORT,
) : Board {
    val base: String = normalize(host)
    @Volatile private var closed = false
    private val fpvd = FpvdClient(URL(base).host, fpvdPort, datagramSocket)
    /** set once dvrhttpd turned out to have no /api/fpv */
    @Volatile var viaFpvd = false
        private set

    override val transport get() = Board.Transport.WIFI
    override val label: String get() = if (viaFpvd) "$base  (settings over fpvd: older board image, no preview)" else base
    override val isAlive: Boolean get() = !closed && networkUp()

    override fun fpv(args: List<String>): String {
        require(args.isNotEmpty())
        args.firstOrNull { !validArg(it) }?.let { throw FpvException(badArgMessage(it)) }
        if (closed) throw IOException("closed")
        if (viaFpvd) return runFpvd(args)
        val c = connect("/api/fpv", READ_TIMEOUT_MS)
        try {
            c.requestMethod = "POST"
            c.doOutput = true
            c.setRequestProperty("X-FPV-Confirm", "1")
            c.setRequestProperty("Content-Type", "text/plain")
            val body = args.joinToString(" ").toByteArray(Charsets.UTF_8)
            c.setFixedLengthStreamingMode(body.size)
            c.outputStream.use { it.write(body) }
            val code = c.responseCode
            val text = (if (code < 400) c.inputStream else c.errorStream)?.use { readText(it) } ?: ""
            // dvrhttpd answers errors as JSON too; anything else is not our board
            if (!isJson(c) && code != 200) throw IOException(notBoard(code))
            if (code == 404) {
                // an older dvrhttpd: recordings only; settings through fpvd
                viaFpvd = true
                return runFpvd(args)
            }
            return text
        } finally {
            c.disconnect()
        }
    }

    private fun runFpvd(args: List<String>): String {
        if (args[0] !in FpvdClient.COMMANDS) throw FpvException(OLD_IMAGE)
        return fpvd.run(args)
    }

    override fun openPreview(port: Int): ChunkStream {
        // dvrhttpd relays its own preview port; [port] only matters on USB
        val c = connect("/live", PREVIEW_READ_TIMEOUT_MS)
        val code = try {
            c.responseCode
        } catch (e: IOException) {
            c.disconnect()
            throw e
        }
        if (code != 200) {
            c.disconnect()
            if (code == 503) throw PreviewNotRunning("preview not running")
            if (code == 404) throw IOException(OLD_IMAGE)
            throw IOException(notBoard(code))
        }
        val input = c.inputStream
        return object : ChunkStream {
            private val buf = ByteArray(64 * 1024)
            override fun read(): ByteArray? {
                val n = input.read(buf)
                return if (n < 0) null else buf.copyOf(n)
            }
            // from another thread too: closes the socket under a blocked read
            override fun close() = c.disconnect()
        }
    }

    override fun download(name: String, out: OutputStream, onProgress: (Long) -> Unit) {
        val c = connect("/dvr/" + URLEncoder.encode(name, "UTF-8"), DOWNLOAD_READ_TIMEOUT_MS)
        try {
            val code = c.responseCode
            if (code != 200) {
                val msg = c.errorStream?.use { readText(it) }.orEmpty()
                throw IOException("$name: HTTP $code ${msg.take(200)}".trim())
            }
            val expect = c.getHeaderField("Content-Length")?.toLongOrNull()
            var total = 0L
            val buf = ByteArray(64 * 1024)
            c.inputStream.use { inp ->
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    total += n
                    onProgress(total)
                }
            }
            if (expect != null && total != expect) throw IOException("$name: got $total of $expect bytes")
        } finally {
            c.disconnect()
        }
    }

    override fun close() {
        closed = true
    }

    private fun connect(path: String, readTimeoutMs: Int): HttpURLConnection {
        if (closed) throw IOException("closed")
        val c = open(URL(base + path))
        c.connectTimeout = CONNECT_TIMEOUT_MS
        c.readTimeout = readTimeoutMs
        c.useCaches = false
        // a 302 is dvrhttpd saying "wrong host", or a captive portal that is not ours
        c.instanceFollowRedirects = false
        return c
    }

    private fun isJson(c: HttpURLConnection) = c.contentType?.startsWith("application/json") == true

    private fun notBoard(code: Int) = "$base answered HTTP $code: is this the board's hotspot?"

    companion object {
        const val DEFAULT_HOST = "10.0.0.1"
        /** what an older board image cannot do over WiFi */
        const val OLD_IMAGE = "this needs a newer board image over WiFi (hotspot web API: /api/fpv, /live); " +
            "settings and recordings work, the preview does not. Use USB, or update the board"
        private const val CONNECT_TIMEOUT_MS = 3000
        /** dvrhttpd gives `fpv` 30 s (restart, reset) */
        private const val READ_TIMEOUT_MS = 35_000
        /** dvrhttpd ends /live after 5 s without a frame */
        private const val PREVIEW_READ_TIMEOUT_MS = 8000
        private const val DOWNLOAD_READ_TIMEOUT_MS = 20_000

        /** "10.0.0.1", "http://10.0.0.1/", "10.0.0.1:8080" -> "http://10.0.0.1[:8080]" */
        fun normalize(host: String): String {
            val h = host.trim().removePrefix("http://").trimEnd('/').ifEmpty { DEFAULT_HOST }
            require(h.all { it.isLetterOrDigit() || it in ".-:[]" }) { "not a host name: $host" }
            return "http://$h"
        }

        /**
         * What dvrhttpd passes on to fpv (fpvcmd_valid_arg in src/fpvcmd.c):
         * 1..64 of letters, digits and _ . , : -  (no shell there, so no quoting).
         */
        fun validArg(s: String): Boolean =
            s.length in 1..64 && s.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it in "_.,:-" }

        fun badArgMessage(s: String): String =
            "'$s' cannot be sent over WiFi: the board accepts only letters, digits and _ . , : - " +
                "(at most 64). Set it over USB instead."

        private fun readText(i: InputStream) = String(i.readBytes(), Charsets.UTF_8)
    }
}
