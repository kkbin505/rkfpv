package io.github.kkbin505.rkfpv.adb

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * One ADB protocol message: a 24-byte little-endian header plus payload.
 * See system/core/adb/protocol.txt in AOSP.
 */
class AdbMessage(
    val command: Int,
    val arg0: Int,
    val arg1: Int,
    val payload: ByteArray = EMPTY,
) {
    fun header(): ByteArray {
        val b = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(command)
        b.putInt(arg0)
        b.putInt(arg1)
        b.putInt(payload.size)
        // Old adbd (protocol 0x01000000) rejects a wrong checksum; newer ones
        // ignore it. Always send it.
        b.putInt(checksum(payload))
        b.putInt(command.inv())
        return b.array()
    }

    override fun toString(): String =
        "${commandName(command)}($arg0, $arg1, ${payload.size} bytes)"

    companion object {
        const val HEADER_SIZE = 24

        const val A_SYNC = 0x434e5953
        const val A_CNXN = 0x4e584e43
        const val A_AUTH = 0x48545541
        const val A_OPEN = 0x4e45504f
        const val A_OKAY = 0x59414b4f
        const val A_CLSE = 0x45534c43
        const val A_WRTE = 0x45545257
        const val A_STLS = 0x534c5453

        const val A_VERSION = 0x01000001
        /** What we accept from the device; the device tells us its own limit in CNXN. */
        const val MAX_PAYLOAD = 256 * 1024

        const val AUTH_TOKEN = 1
        const val AUTH_SIGNATURE = 2
        const val AUTH_RSAPUBLICKEY = 3

        val EMPTY = ByteArray(0)

        fun checksum(data: ByteArray): Int {
            var sum = 0
            for (x in data) sum += x.toInt() and 0xff
            return sum
        }

        fun commandName(c: Int): String = when (c) {
            A_SYNC -> "SYNC"; A_CNXN -> "CNXN"; A_AUTH -> "AUTH"; A_OPEN -> "OPEN"
            A_OKAY -> "OKAY"; A_CLSE -> "CLSE"; A_WRTE -> "WRTE"; A_STLS -> "STLS"
            else -> "0x%08x".format(c)
        }

        /** Parsed header fields; payload is read separately (its length is in [length]). */
        class Header(val command: Int, val arg0: Int, val arg1: Int, val length: Int)

        fun parseHeader(h: ByteArray, maxPayload: Int): Header {
            val b = ByteBuffer.wrap(h, 0, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            val command = b.int
            val arg0 = b.int
            val arg1 = b.int
            val length = b.int
            b.int // checksum: not checked (newer adbd sends 0)
            val magic = b.int
            if (magic != command.inv()) {
                throw IOException("ADB: bad message header (out of sync); unplug and reconnect")
            }
            if (length < 0 || length > maxPayload) {
                throw IOException("ADB: ${commandName(command)} payload of $length bytes is too large")
            }
            return Header(command, arg0, arg1, length)
        }
    }
}
