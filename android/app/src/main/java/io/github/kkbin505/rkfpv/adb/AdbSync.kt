package io.github.kkbin505.rkfpv.adb

import java.io.Closeable
import java.io.EOFException
import java.io.IOException
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The "sync:" service: what `adb pull` uses. Requests and replies are
 * 8-byte headers (4-char id + little-endian u32) that may span or share
 * WRTE packets, so replies are read through a small buffer.
 */
class AdbSync internal constructor(private val stream: AdbStream) : Closeable {

    class Stat(val mode: Int, val size: Long, val mtime: Long) {
        val exists: Boolean get() = mode != 0
    }

    private var buf = ByteArray(0)
    private var pos = 0

    fun stat(path: String): Stat {
        request("STAT", path)
        val r = readBytes(16)
        val b = ByteBuffer.wrap(r).order(ByteOrder.LITTLE_ENDIAN)
        val id = String(r, 0, 4, Charsets.US_ASCII)
        if (id != "STAT") throw IOException("ADB sync: unexpected '$id' to STAT")
        b.position(4)
        val mode = b.int
        val size = b.int.toLong() and 0xffffffffL
        val mtime = b.int.toLong() and 0xffffffffL
        return Stat(mode, size, mtime)
    }

    /** Copy a device file into [out]; [onProgress] gets the bytes so far. Returns the size. */
    fun pull(path: String, out: OutputStream, onProgress: (Long) -> Unit = {}): Long {
        request("RECV", path)
        var total = 0L
        val chunk = ByteArray(MAX_DATA)
        while (true) {
            val h = readBytes(8)
            val id = String(h, 0, 4, Charsets.US_ASCII)
            val len = ByteBuffer.wrap(h, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
            when (id) {
                "DATA" -> {
                    if (len < 0 || len > MAX_DATA) throw IOException("ADB sync: DATA of $len bytes")
                    readExact(chunk, 0, len)
                    out.write(chunk, 0, len)
                    total += len
                    onProgress(total)
                }
                "DONE" -> return total
                "FAIL" -> {
                    val msg = String(readBytes(len), Charsets.UTF_8)
                    throw IOException("$path: $msg")
                }
                else -> throw IOException("ADB sync: unexpected '$id'")
            }
        }
    }

    private fun request(id: String, path: String) {
        val p = path.toByteArray(Charsets.UTF_8)
        val b = ByteBuffer.allocate(8 + p.size).order(ByteOrder.LITTLE_ENDIAN)
        b.put(id.toByteArray(Charsets.US_ASCII))
        b.putInt(p.size)
        b.put(p)
        stream.write(b.array())
    }

    private fun readBytes(n: Int): ByteArray = ByteArray(n).also { readExact(it, 0, n) }

    private fun readExact(out: ByteArray, off: Int, len: Int) {
        var done = 0
        while (done < len) {
            if (pos >= buf.size) {
                buf = stream.read(TIMEOUT_MS) ?: throw EOFException("ADB sync: stream ended")
                pos = 0
                continue
            }
            val n = minOf(len - done, buf.size - pos)
            System.arraycopy(buf, pos, out, off + done, n)
            pos += n
            done += n
        }
    }

    override fun close() {
        runCatching { request("QUIT", "") }
        stream.close()
    }

    private companion object {
        const val MAX_DATA = 64 * 1024
        const val TIMEOUT_MS = 20_000L
    }
}
