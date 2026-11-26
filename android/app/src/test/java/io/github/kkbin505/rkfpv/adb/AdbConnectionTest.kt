package io.github.kkbin505.rkfpv.adb

import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_RSAPUBLICKEY
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_SIGNATURE
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_TOKEN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_AUTH
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_CLSE
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_CNXN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_OKAY
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_OPEN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_VERSION
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_WRTE
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.math.BigInteger
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicReference
import kotlin.random.Random

/**
 * The host side against a fake adbd on a local socket. The fake follows
 * the protocol strictly (every WRTE must be OKAYed before the next one), uses
 * a small max payload so sync headers straddle WRTE packets, and checks the
 * AUTH signature with the public key the way adbd does.
 */
class AdbConnectionTest {

    private val key = AdbKey.fromPem(javaClass.classLoader!!.getResource("adb/test_adbkey")!!.readText())
    private val file = Random(7).nextBytes(200_000)
    private val server = ServerSocket(0)
    private val deviceError = AtomicReference<Throwable?>(null)
    private var conn: AdbConnection? = null

    @After fun tearDown() {
        conn?.close()
        server.close()
        deviceError.get()?.let { throw AssertionError("fake device failed", it) }
    }

    private fun connect(knowsKey: Boolean, onAwait: () -> Unit = {}): AdbConnection {
        val t = Thread { runCatching { FakeDevice(server.accept(), knowsKey).run() }.onFailure { if (it !is IOException) deviceError.set(it) } }
        t.isDaemon = true
        t.start()
        return AdbConnection.connect(TcpAdbChannel("127.0.0.1", server.localPort), key, "test@host",
            onAwaitingAuthorization = onAwait).also { conn = it }
    }

    @Test fun signatureAccepted() {
        var asked = false
        val c = connect(knowsKey = true) { asked = true }
        assertEquals("device::ro.product.name=fake;", c.banner)
        assertEquals(4096, c.maxData)
        assertTrue(!asked)
    }

    @Test fun unknownKeySendsPublicKey() {
        var asked = false
        connect(knowsKey = false) { asked = true }
        assertTrue(asked)
    }

    @Test fun shellOutputAcrossPackets() {
        val c = connect(true)
        val out = c.shell("big")
        assertEquals(BIG_OUTPUT.replace("\r\n", "\n"), out)
        // the connection is still usable afterwards
        assertEquals("hi\n", c.shell("echo hi"))
    }

    @Test fun refusedService() {
        val c = connect(true)
        try {
            c.open("nope:")
            fail("expected refusal")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("refused"))
        }
    }

    @Test fun syncStatAndPull() {
        val c = connect(true)
        c.sync().use { s ->
            val st = s.stat("/mnt/dvr/rec_0001_01.mp4")
            assertEquals(file.size.toLong(), st.size)
            assertTrue(st.exists)
            val out = ByteArrayOutputStream()
            var last = 0L
            val n = s.pull("/mnt/dvr/rec_0001_01.mp4", out) { last = it }
            assertEquals(file.size.toLong(), n)
            assertEquals(n, last)
            assertArrayEquals(file, out.toByteArray())
        }
    }

    @Test fun syncPullMissingFile() {
        val c = connect(true)
        c.sync().use { s ->
            try {
                s.pull("/mnt/dvr/missing.mp4", ByteArrayOutputStream())
                fail("expected FAIL")
            } catch (e: IOException) {
                assertTrue(e.message!!.contains("No such file"))
            }
        }
    }

    /** The preview reads with no timeout on its own thread; stop() closes the stream from another. */
    @Test fun closeWakesABlockedReader() {
        val c = connect(true)
        val s = c.open("shell:silent")
        var result: Any? = "not returned"
        val t = Thread { result = runCatching { s.read(0) }.getOrNull() }
        t.start()
        Thread.sleep(200)
        s.close()
        t.join(2000)
        assertTrue("reader still blocked", !t.isAlive)
        assertEquals(null, result)
        // the connection is fine afterwards
        assertEquals("hi\n", c.shell("echo hi"))
    }

    @Test fun deviceGoneFailsTheConnection() {
        val c = connect(true)
        try {
            c.shell("die")
            fail("expected failure")
        } catch (e: IOException) {
            // expected
        }
        Thread.sleep(100)
        assertTrue(!c.isAlive)
    }

    companion object {
        val BIG_OUTPUT = (1..400).joinToString("") { "line $it of the shell output\r\n" }
    }

    /** Minimal adbd: one client, services handled one at a time. */
    private inner class FakeDevice(private val sock: Socket, private val knowsKey: Boolean) {
        private val ch = object : AdbChannel {
            val i = sock.getInputStream()
            val o = sock.getOutputStream()
            override fun write(buf: ByteArray) { o.write(buf); o.flush() }
            override fun readFully(buf: ByteArray, off: Int, len: Int, timeoutMs: Int) {
                var d = 0
                while (d < len) {
                    val n = i.read(buf, off + d, len - d)
                    if (n < 0) throw IOException("host closed")
                    d += n
                }
            }
            override fun close() = sock.close()
        }
        private val maxData = 4096
        private var nextId = 100

        private fun send(m: AdbMessage) {
            ch.write(m.header())
            if (m.payload.isNotEmpty()) ch.write(m.payload)
        }

        private fun read(): AdbMessage = AdbConnection.readMessage(ch, 1 shl 20, 0)

        private fun expect(cmd: Int): AdbMessage {
            val m = read()
            assertEquals(AdbMessage.commandName(cmd), AdbMessage.commandName(m.command))
            return m
        }

        private fun verify(token: ByteArray, sig: ByteArray): Boolean {
            val m = BigInteger(1, sig).modPow(key.publicExponent, key.modulus).toByteArray()
            val digestInfo = BigInteger("3021300906052b0e03021a05000414", 16).toByteArray()
            return m.takeLast(35) == (digestInfo + token).toList() && m[0] == 1.toByte()
        }

        fun run() {
            val cnxn = expect(A_CNXN)
            assertEquals(A_VERSION, cnxn.arg0)
            val token = Random(3).nextBytes(20)
            send(AdbMessage(A_AUTH, AUTH_TOKEN, 0, token))
            val sig = expect(A_AUTH)
            assertEquals(AUTH_SIGNATURE, sig.arg0)
            if (knowsKey) {
                assertTrue("bad signature", verify(token, sig.payload))
            } else {
                send(AdbMessage(A_AUTH, AUTH_TOKEN, 0, Random(4).nextBytes(20)))
                val pk = expect(A_AUTH)
                assertEquals(AUTH_RSAPUBLICKEY, pk.arg0)
                assertArrayEquals(key.publicKeyPayload("test@host"), pk.payload)
            }
            send(AdbMessage(A_CNXN, A_VERSION, maxData, "device::ro.product.name=fake;\u0000".toByteArray()))

            while (true) {
                val m = read()
                if (m.command != A_OPEN) continue   // stray OKAY / CLSE
                val dest = String(m.payload).trimEnd('\u0000')
                val host = m.arg0
                val me = nextId++
                when {
                    dest == "shell:big" -> { send(AdbMessage(A_OKAY, me, host)); writeAll(me, host, BIG_OUTPUT.toByteArray()); close(me, host) }
                    dest == "shell:echo hi" -> { send(AdbMessage(A_OKAY, me, host)); writeAll(me, host, "hi\r\n".toByteArray()); close(me, host) }
                    dest == "shell:silent" -> send(AdbMessage(A_OKAY, me, host))
                    dest == "shell:die" -> { send(AdbMessage(A_OKAY, me, host)); sock.close(); return }
                    dest == "sync:" -> { send(AdbMessage(A_OKAY, me, host)); sync(me, host) }
                    else -> send(AdbMessage(A_CLSE, 0, host))
                }
            }
        }

        private fun writeAll(me: Int, host: Int, data: ByteArray) {
            var off = 0
            while (off < data.size) {
                val n = minOf(maxData, data.size - off)
                send(AdbMessage(A_WRTE, me, host, data.copyOfRange(off, off + n)))
                val ok = expect(A_OKAY)   // strict flow control
                assertEquals(host, ok.arg0)
                assertEquals(me, ok.arg1)
                off += n
            }
        }

        private fun close(me: Int, host: Int) {
            send(AdbMessage(A_CLSE, me, host))
            // host answers with nothing or its own CLSE; both fine
        }

        private val pending = ByteArrayOutputStream()
        private var pbuf = ByteArray(0)
        private var ppos = 0

        private fun syncRead(n: Int, me: Int, host: Int): ByteArray? {
            while (pbuf.size - ppos < n) {
                val m = read()
                when (m.command) {
                    A_WRTE -> {
                        send(AdbMessage(A_OKAY, me, host))
                        pbuf = pbuf.copyOfRange(ppos, pbuf.size) + m.payload
                        ppos = 0
                    }
                    A_CLSE -> return null
                    else -> {}
                }
            }
            return pbuf.copyOfRange(ppos, ppos + n).also { ppos += n }
        }

        private fun le(v: Int) = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array()

        private fun sync(me: Int, host: Int) {
            while (true) {
                val h = syncRead(8, me, host) ?: return
                val id = String(h, 0, 4)
                val len = ByteBuffer.wrap(h, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
                val path = String(syncRead(len, me, host) ?: return)
                pending.reset()
                when (id) {
                    "STAT" -> pending.write("STAT".toByteArray() + le(0x81a4) + le(file.size) + le(1700000000))
                    "RECV" -> if (path.endsWith("rec_0001_01.mp4")) {
                        var off = 0
                        while (off < file.size) {
                            val n = minOf(65536, file.size - off)
                            pending.write("DATA".toByteArray() + le(n))
                            pending.write(file, off, n)
                            off += n
                        }
                        pending.write("DONE".toByteArray() + le(0))
                    } else {
                        val msg = "No such file or directory".toByteArray()
                        pending.write("FAIL".toByteArray() + le(msg.size) + msg)
                    }
                    "QUIT" -> { close(me, host); return }
                }
                writeAll(me, host, pending.toByteArray())
            }
        }
    }
}
