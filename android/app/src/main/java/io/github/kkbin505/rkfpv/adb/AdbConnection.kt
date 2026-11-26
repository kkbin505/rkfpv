package io.github.kkbin505.rkfpv.adb

import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_RSAPUBLICKEY
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_SIGNATURE
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.AUTH_TOKEN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_AUTH
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_CLSE
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_CNXN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_OKAY
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_OPEN
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_STLS
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_VERSION
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.A_WRTE
import io.github.kkbin505.rkfpv.adb.AdbMessage.Companion.MAX_PAYLOAD
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * An ADB host connection to one device: the same protocol the `adb` server
 * on a PC speaks, so the board needs nothing new. One reader thread hands
 * incoming messages to the open streams.
 */
class AdbConnection private constructor(
    private val channel: AdbChannel,
    /** Protocol version the device announced. */
    val deviceVersion: Int,
    /** Largest payload we may send (the smaller of the two sides' limits). */
    val maxData: Int,
    /** Device banner, e.g. "device::ro.product.name=...;". */
    val banner: String,
) : Closeable {

    private val writeLock = Any()
    private val streams = ConcurrentHashMap<Int, AdbStream>()
    private val nextId = AtomicInteger(1)

    @Volatile private var failure: IOException? = null
    @Volatile var isClosed = false
        private set

    /** Becomes false when the cable is pulled or the board reboots. */
    val isAlive: Boolean get() = !isClosed && failure == null

    private val reader = Thread(::readLoop, "adb-reader").apply { isDaemon = true }

    internal fun send(m: AdbMessage) {
        failure?.let { throw IOException(it.message, it) }
        if (isClosed) throw IOException("ADB: connection closed")
        synchronized(writeLock) {
            channel.write(m.header())
            if (m.payload.isNotEmpty()) channel.write(m.payload)
        }
    }

    /** Open a service on the device, e.g. "shell:ls" or "sync:". */
    fun open(destination: String, timeoutMs: Long = 5000): AdbStream {
        val id = nextId.getAndIncrement()
        val s = AdbStream(this, id, destination)
        streams[id] = s
        try {
            send(AdbMessage(A_OPEN, id, 0, (destination + "\u0000").toByteArray(Charsets.UTF_8)))
            s.awaitOpen(timeoutMs)
        } catch (e: Exception) {
            streams.remove(id)
            throw e
        }
        return s
    }

    /** Run a command with the device's shell and return what it printed (LF line ends). */
    fun shell(command: String, timeoutMs: Long = 20_000): String =
        open("shell:$command").use { s ->
            String(s.readAll(timeoutMs), Charsets.UTF_8).replace("\r\n", "\n")
        }

    /** File transfer service (adb pull/push protocol). */
    fun sync(): AdbSync = AdbSync(open("sync:"))

    internal fun removeStream(id: Int) {
        streams.remove(id)
    }

    private fun readLoop() {
        try {
            while (!isClosed) {
                val m = readMessage(channel, maxOf(maxData, MAX_PAYLOAD), 0)
                when (m.command) {
                    A_OKAY -> streams[m.arg1]?.onOkay(m.arg0)
                    A_WRTE -> {
                        val s = streams[m.arg1]
                        if (s != null) s.onData(m.payload)
                        else send(AdbMessage(A_CLSE, 0, m.arg0))
                    }
                    A_CLSE -> streams[m.arg1]?.onRemoteClose()
                    A_CNXN, A_AUTH -> throw IOException("ADB: the board restarted the connection")
                    else -> {}
                }
            }
        } catch (e: IOException) {
            if (!isClosed) failure = e
        } finally {
            val err = failure ?: IOException("ADB: connection closed")
            for (s in streams.values) s.onFailure(err)
        }
    }

    override fun close() {
        if (isClosed) return
        for (s in streams.values.toList()) runCatching { s.close() }
        isClosed = true
        runCatching { channel.close() }
    }

    companion object {
        /**
         * CNXN / AUTH handshake. [onAwaitingAuthorization] is called when the
         * device did not know our key and we sent it the public key: a device
         * with a screen asks its user now; the board normally has auth off.
         */
        fun connect(
            channel: AdbChannel,
            key: AdbKey,
            keyName: String,
            authTimeoutMs: Int = 30_000,
            onAwaitingAuthorization: () -> Unit = {},
        ): AdbConnection {
            try {
                fun send(m: AdbMessage) {
                    channel.write(m.header())
                    if (m.payload.isNotEmpty()) channel.write(m.payload)
                }
                send(AdbMessage(A_CNXN, A_VERSION, MAX_PAYLOAD, "host::\u0000".toByteArray()))
                var sentSignature = false
                var sentKey = false
                while (true) {
                    val m = readMessage(channel, MAX_PAYLOAD, if (sentKey) authTimeoutMs else 5000)
                    when (m.command) {
                        A_CNXN -> {
                            val banner = String(m.payload, Charsets.UTF_8).trimEnd('\u0000')
                            val c = AdbConnection(channel, m.arg0, minOf(m.arg1, MAX_PAYLOAD), banner)
                            c.reader.start()
                            return c
                        }
                        A_AUTH -> {
                            if (m.arg0 != AUTH_TOKEN) continue
                            when {
                                !sentSignature -> {
                                    send(AdbMessage(A_AUTH, AUTH_SIGNATURE, 0, key.sign(m.payload)))
                                    sentSignature = true
                                }
                                !sentKey -> {
                                    send(AdbMessage(A_AUTH, AUTH_RSAPUBLICKEY, 0, key.publicKeyPayload(keyName)))
                                    sentKey = true
                                    onAwaitingAuthorization()
                                }
                                else -> throw IOException("ADB: the board did not accept this phone's key")
                            }
                        }
                        A_STLS -> throw IOException("ADB: the device wants TLS (wireless debugging); not supported")
                        else -> {} // left over from an earlier session: skip
                    }
                }
            } catch (e: Exception) {
                runCatching { channel.close() }
                throw e
            }
        }

        internal fun readMessage(ch: AdbChannel, maxPayload: Int, timeoutMs: Int): AdbMessage {
            val h = ByteArray(AdbMessage.HEADER_SIZE)
            ch.readFully(h, 0, h.size, timeoutMs)
            val hd = AdbMessage.parseHeader(h, maxPayload)
            val p = if (hd.length == 0) AdbMessage.EMPTY
            else ByteArray(hd.length).also { ch.readFully(it, 0, it.size, timeoutMs) }
            return AdbMessage(hd.command, hd.arg0, hd.arg1, p)
        }
    }
}

/** One open service on the device. Reads are acknowledged one by one, so a slow reader slows the device. */
class AdbStream internal constructor(
    private val conn: AdbConnection,
    val localId: Int,
    val destination: String,
) : Closeable {

    @Volatile private var remoteId = 0
    private val okays = LinkedBlockingQueue<Int>()
    private val data = LinkedBlockingQueue<ByteArray>()
    @Volatile private var remoteClosed = false
    @Volatile private var failure: IOException? = null
    @Volatile private var closed = false
    private var eofSeen = false

    internal fun onOkay(remote: Int) {
        okays.offer(remote)
    }

    internal fun onData(p: ByteArray) {
        data.offer(p)
    }

    internal fun onRemoteClose() {
        remoteClosed = true
        okays.offer(CLOSED)
        data.offer(EOF)
    }

    internal fun onFailure(e: IOException) {
        if (failure == null) failure = e
        okays.offer(CLOSED)
        data.offer(EOF)
    }

    internal fun awaitOpen(timeoutMs: Long) {
        val r = okays.poll(timeoutMs, TimeUnit.MILLISECONDS)
            ?: throw AdbTimeoutException("ADB: '$destination' did not open")
        if (r == CLOSED) {
            failure?.let { throw it }
            throw IOException("ADB: the board refused '$destination'")
        }
        remoteId = r
    }

    fun write(bytes: ByteArray, timeoutMs: Long = 10_000) {
        var off = 0
        while (off < bytes.size) {
            if (closed || remoteClosed) throw failure ?: IOException("ADB: stream closed")
            val n = minOf(conn.maxData, bytes.size - off)
            conn.send(AdbMessage(A_WRTE, localId, remoteId, bytes.copyOfRange(off, off + n)))
            val r = okays.poll(timeoutMs, TimeUnit.MILLISECONDS)
                ?: throw AdbTimeoutException("ADB: '$destination' stopped taking data")
            if (r == CLOSED) throw failure ?: IOException("ADB: stream closed by the board")
            off += n
        }
    }

    /** Next chunk from the device; null at the end of the stream. [timeoutMs] = 0 waits for ever. */
    fun read(timeoutMs: Long): ByteArray? {
        if (eofSeen) return null
        val p = (if (timeoutMs <= 0) data.take() else data.poll(timeoutMs, TimeUnit.MILLISECONDS))
            ?: throw AdbTimeoutException("ADB: '$destination': no data in $timeoutMs ms")
        if (p === EOF) {
            eofSeen = true
            failure?.let { throw it }
            return null
        }
        // ready for the next WRTE
        if (!remoteClosed && !closed) conn.send(AdbMessage(A_OKAY, localId, remoteId))
        return p
    }

    /** Everything until the device closes the stream. */
    fun readAll(timeoutMs: Long): ByteArray {
        val out = ByteArrayOutputStream()
        val deadline = System.currentTimeMillis() + timeoutMs
        while (true) {
            val left = deadline - System.currentTimeMillis()
            if (left <= 0) throw AdbTimeoutException("ADB: '$destination' took longer than $timeoutMs ms")
            val p = read(left) ?: return out.toByteArray()
            out.write(p)
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        conn.removeStream(localId)
        if (!remoteClosed && remoteId != 0 && conn.isAlive) {
            runCatching { conn.send(AdbMessage(A_CLSE, localId, remoteId)) }
        }
    }

    private companion object {
        const val CLOSED = Int.MIN_VALUE
        val EOF = ByteArray(0)
    }
}
