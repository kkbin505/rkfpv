package io.github.kkbin505.rkfpv.board

import io.github.kkbin505.rkfpv.fpv.FpvClient
import io.github.kkbin505.rkfpv.fpv.FpvException
import io.github.kkbin505.rkfpv.video.AuReader
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.concurrent.thread

/**
 * HttpBoard against a fake dvrhttpd that answers like src/dvrhttpd.c:
 * JSON errors, 503 on /live while the preview is off, the confirm header.
 */
class HttpBoardTest {

    private class Req(val method: String, val path: String, val headers: Map<String, String>, val body: String)

    /** [chunks] are written one by one with a flush between (a split-up stream). */
    private class Resp(val code: Int, val type: String, val chunks: List<ByteArray>, val length: Boolean = true) {
        constructor(code: Int, type: String, body: String) : this(code, type, listOf(body.toByteArray()))
    }

    private class FakeServer(val handler: (Req) -> Resp) : Closeable {
        val socket = ServerSocket(0, 50, InetAddress.getLoopbackAddress())
        val requests = CopyOnWriteArrayList<Req>()
        val host get() = "127.0.0.1:${socket.localPort}"

        init {
            thread(isDaemon = true) {
                while (!socket.isClosed) {
                    val c = runCatching { socket.accept() }.getOrNull() ?: break
                    thread(isDaemon = true) { c.use { serve(it) } }
                }
            }
        }

        private fun serve(c: Socket) {
            val inp = c.getInputStream()
            val head = readHead(inp) ?: return
            val lines = head.split("\r\n")
            val (method, path) = lines[0].split(" ")
            val headers = lines.drop(1).filter { ':' in it }
                .associate { it.substringBefore(':').trim().lowercase() to it.substringAfter(':').trim() }
            val n = headers["content-length"]?.toInt() ?: 0
            val body = ByteArray(n)
            var got = 0
            while (got < n) {
                val r = inp.read(body, got, n - got)
                if (r < 0) break
                got += r
            }
            val req = Req(method, path, headers, String(body, 0, got))
            requests += req
            val resp = handler(req)
            val out = c.getOutputStream()
            val total = resp.chunks.sumOf { it.size }
            out.write(("HTTP/1.1 ${resp.code} X\r\nContent-Type: ${resp.type}\r\n" +
                (if (resp.length) "Content-Length: $total\r\n" else "") + "Connection: close\r\n\r\n").toByteArray())
            out.flush()
            for (ch in resp.chunks) {
                out.write(ch)
                out.flush()
                Thread.sleep(2)
            }
        }

        private fun readHead(inp: InputStream): String? {
            val b = ByteArrayOutputStream()
            while (true) {
                val x = inp.read()
                if (x < 0) return null
                b.write(x)
                val s = b.toString()
                if (s.endsWith("\r\n\r\n")) return s.removeSuffix("\r\n\r\n")
            }
        }

        override fun close() = socket.close()
    }

    private var server: FakeServer? = null

    private fun board(handler: (Req) -> Resp): Pair<HttpBoard, FakeServer> {
        val s = FakeServer(handler)
        server = s
        return HttpBoard(s.host) to s
    }

    @After fun tearDown() {
        server?.close()
    }

    private val setReply = """{"ok":true,"fpv_running":false,"results":[{"key":"bitrate","value":"4000","apply":"venc","result":"saved"}],"warning":""}"""

    @Test fun fpvPostsTheArgumentsWithTheConfirmHeader() {
        val (b, s) = board { Resp(200, "application/json", setReply) }
        val r = FpvClient { b.fpv(it) }.set(linkedMapOf("bitrate" to "4000"))
        assertEquals("saved", r.results.single().result)
        val q = s.requests.single()
        assertEquals("POST", q.method)
        assertEquals("/api/fpv", q.path)
        assertEquals("1", q.headers["x-fpv-confirm"])
        // dvrhttpd adds --json itself and runs fpv without a shell: no quoting
        assertEquals("set bitrate 4000", q.body)
        assertEquals(s.host, q.headers["host"])
    }

    @Test fun boardErrorsComeBackAsFpvErrors() {
        val (b, _) = board { Resp(403, "application/json", """{"ok":false,"error":"missing X-FPV-Confirm header"}""") }
        try {
            FpvClient { b.fpv(it) }.status()
            fail()
        } catch (e: FpvException) {
            assertEquals("missing X-FPV-Confirm header", e.message)
        }
    }

    @Test fun somethingElseAtTheAddressIsNotTheBoard() {
        // a home router on 10.0.0.1, say
        val (b, _) = board { Resp(404, "text/html", "<html>router login</html>") }
        try {
            b.fpv(listOf("status"))
            fail()
        } catch (e: IOException) {
            assertTrue(e.message!!, "hotspot" in e.message!!)
        }
    }

    /**
     * A fake fpvd: drops the first datagram of each id (a lost packet), then
     * answers [reply] in two chunks, newest-first, like a reordering link.
     */
    private class FakeFpvd(val reply: String) : Closeable {
        val socket = DatagramSocket(0, InetAddress.getLoopbackAddress())
        val requests = CopyOnWriteArrayList<String>()

        init {
            thread(isDaemon = true) {
                val buf = ByteArray(2048)
                val seen = HashSet<String>()
                while (!socket.isClosed) {
                    val p = DatagramPacket(buf, buf.size)
                    runCatching { socket.receive(p) }.getOrNull() ?: break
                    val req = String(buf, 0, p.length)
                    requests += req
                    val id = req.split(" ")[1]
                    if (seen.add(id)) continue
                    val half = reply.length / 2
                    val chunks = listOf(reply.substring(0, half), reply.substring(half))
                    for (i in chunks.indices.reversed()) {
                        val d = "FPV1 $id ${i + 1}/2\n${chunks[i]}".toByteArray()
                        socket.send(DatagramPacket(d, d.size, p.socketAddress))
                    }
                }
            }
        }

        override fun close() = socket.close()
    }

    @Test fun anOlderImageGetsItsSettingsThroughFpvd() {
        // hotspot images before the web API: dvrhttpd serves recordings only, JSON 404 for the rest
        val udp = FakeFpvd(setReply)
        try {
            val s = FakeServer { Resp(404, "application/json", """{"ok":false,"error":"not found"}""") }
            server = s
            val b = HttpBoard(s.host, fpvdPort = udp.socket.localPort)
            val r = FpvClient { b.fpv(it) }.set(linkedMapOf("bitrate" to "4000"))
            assertEquals("4000", r.results.single().value)
            assertTrue(b.viaFpvd)
            // the lost first datagram was resent with the same id
            assertEquals(2, udp.requests.size)
            assertEquals(udp.requests[0], udp.requests[1])
            assertTrue(udp.requests[0].matches(Regex("FPV1 [0-9a-f]{12} set bitrate 4000")))
            // from now on straight to fpvd
            FpvClient { b.fpv(it) }.set(linkedMapOf("bitrate" to "4000"))
            assertEquals(1, s.requests.size)
            // what fpvd does not run, and the preview, need the newer image
            try {
                b.fpv(listOf("preview", "on"))
                fail()
            } catch (e: FpvException) {
                assertEquals(HttpBoard.OLD_IMAGE, e.message)
            }
            try {
                b.openPreview(5602)
                fail()
            } catch (e: IOException) {
                assertEquals(HttpBoard.OLD_IMAGE, e.message)
            }
        } finally {
            udp.close()
        }
    }

    @Test fun fpvdThatNeverAnswers() {
        val dead = DatagramSocket(0, InetAddress.getLoopbackAddress())
        try {
            val c = FpvdClient("127.0.0.1", dead.localPort, tries = 2, tryTimeoutMs = 200)
            c.run(listOf("status"))
            fail()
        } catch (e: IOException) {
            assertTrue(e.message!!, "fpvd" in e.message!!)
        } finally {
            dead.close()
        }
    }

    @Test fun argumentsDvrhttpdWouldRefuseAreNotSent() {
        val (b, s) = board { Resp(200, "application/json", setReply) }
        for (bad in listOf("a b", "/userdata/iq", "", "x".repeat(65), "p@ss", "'\$(reboot)'")) {
            try {
                b.fpv(listOf("set", "hotspot_pass", bad))
                fail(bad)
            } catch (e: FpvException) {
                assertTrue("USB" in e.message!!)
            }
        }
        assertTrue(s.requests.isEmpty())
        // what it does accept (fpvcmd_valid_arg)
        for (ok in listOf("1280x720", "auto", "-5", "rec_0003_01.mp4", "a,b:c", "x".repeat(64))) {
            assertTrue(ok, HttpBoard.validArg(ok))
        }
    }

    @Test fun previewNotRunningIs503() {
        val (b, _) = board { Resp(503, "application/json", """{"ok":false,"error":"preview not running (fpv preview on)"}""") }
        try {
            b.openPreview(5602)
            fail()
        } catch (e: PreviewNotRunning) {
        }
    }

    @Test fun previewStreamSplitAnywhereGivesTheFrames() {
        val frames = listOf(ByteArray(3000) { (it % 251).toByte() }, ByteArray(10) { 7 }, ByteArray(70_000) { (it * 3).toByte() })
        val wire = ByteArrayOutputStream()
        for (f in frames) {
            wire.write(byteArrayOf((f.size ushr 24).toByte(), (f.size ushr 16).toByte(), (f.size ushr 8).toByte(), f.size.toByte()))
            wire.write(f)
        }
        val all = wire.toByteArray()
        // cut inside a length header and inside frames
        val cuts = listOf(0, 2, 1500, 3004, 3007, 3020, 40_000, all.size)
        val chunks = cuts.zipWithNext { a, z -> all.copyOfRange(a, z) }
        val (b, s) = board { Resp(200, "application/octet-stream", chunks, length = false) }
        b.openPreview(5602).use { st ->
            val r = AuReader { st.read() }
            for (f in frames) assertArrayEquals(f, r.read())
            assertEquals(null, r.read())
        }
        assertEquals("/live", s.requests.single().path)
    }

    @Test fun downloadCopiesTheFile() {
        val data = ByteArray(300_000) { (it % 13).toByte() }
        val (b, s) = board { q ->
            if (q.path == "/dvr/rec_0003_01.mp4") Resp(200, "video/mp4", listOf(data.copyOfRange(0, 1000), data.copyOfRange(1000, data.size)))
            else Resp(404, "application/json", """{"ok":false,"error":"not a recording name"}""")
        }
        val out = ByteArrayOutputStream()
        var last = 0L
        b.download("rec_0003_01.mp4", out) { last = it }
        assertArrayEquals(data, out.toByteArray())
        assertEquals(data.size.toLong(), last)
        assertEquals("GET", s.requests.single().method)
        try {
            b.download("rec_0009_01.mp4", ByteArrayOutputStream()) {}
            fail()
        } catch (e: IOException) {
            assertTrue(e.message!!, "404" in e.message!!)
        }
    }

    @Test fun closedBoardIsDead() {
        val (b, s) = board { Resp(200, "application/json", setReply) }
        assertTrue(b.isAlive)
        b.close()
        assertEquals(false, b.isAlive)
        try {
            b.fpv(listOf("status"))
            fail()
        } catch (e: IOException) {
        }
        assertTrue(s.requests.isEmpty())
    }

    @Test fun addresses() {
        assertEquals("http://10.0.0.1", HttpBoard.normalize("10.0.0.1"))
        assertEquals("http://10.0.0.1", HttpBoard.normalize(" http://10.0.0.1/ "))
        assertEquals("http://10.0.0.1", HttpBoard.normalize(""))
        assertEquals("http://127.0.0.1:8080", HttpBoard.normalize("127.0.0.1:8080"))
        try {
            HttpBoard.normalize("10.0.0.1/../x")
            fail()
        } catch (e: IllegalArgumentException) {
        }
    }
}
