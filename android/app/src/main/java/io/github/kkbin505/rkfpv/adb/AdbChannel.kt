package io.github.kkbin505.rkfpv.adb

import java.io.Closeable
import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Byte transport under the ADB protocol: USB bulk endpoints on the phone,
 * a TCP socket in tests (or for an adbd listening on a network port).
 */
interface AdbChannel : Closeable {
    /** Write one header or one payload, all of it. */
    fun write(buf: ByteArray)

    /**
     * Read exactly [len] bytes. [timeoutMs] = 0 waits for ever.
     * Throws [AdbTimeoutException] when nothing arrived in time.
     */
    fun readFully(buf: ByteArray, off: Int, len: Int, timeoutMs: Int)
}

class AdbTimeoutException(msg: String) : java.io.IOException(msg)

class TcpAdbChannel(host: String, port: Int, connectTimeoutMs: Int = 3000) : AdbChannel {
    private val socket = Socket()
    private val input: InputStream
    private val output: OutputStream

    init {
        socket.tcpNoDelay = true
        socket.connect(InetSocketAddress(host, port), connectTimeoutMs)
        input = socket.getInputStream()
        output = socket.getOutputStream()
    }

    override fun write(buf: ByteArray) {
        output.write(buf)
        output.flush()
    }

    override fun readFully(buf: ByteArray, off: Int, len: Int, timeoutMs: Int) {
        socket.soTimeout = timeoutMs
        var done = 0
        while (done < len) {
            val n = try {
                input.read(buf, off + done, len - done)
            } catch (e: SocketTimeoutException) {
                throw AdbTimeoutException("ADB: no answer in $timeoutMs ms")
            }
            if (n < 0) throw EOFException("ADB: connection closed")
            done += n
        }
    }

    override fun close() {
        socket.close()
    }
}
