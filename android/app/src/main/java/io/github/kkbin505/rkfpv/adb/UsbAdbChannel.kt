package io.github.kkbin505.rkfpv.adb

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import java.io.IOException

/**
 * ADB over the phone's USB host port (Android USB Host API, no root).
 * The board is a USB gadget with an ADB interface (class 0xff, subclass
 * 0x42, protocol 1), the same one `adb` on a PC talks to.
 */
class UsbAdbChannel private constructor(
    private val conn: UsbDeviceConnection,
    private val iface: UsbInterface,
    private val epIn: UsbEndpoint,
    private val epOut: UsbEndpoint,
) : AdbChannel {

    @Volatile private var closed = false

    override fun write(buf: ByteArray) {
        var off = 0
        while (off < buf.size) {
            val n = minOf(CHUNK, buf.size - off)
            val r = conn.bulkTransfer(epOut, buf, off, n, WRITE_TIMEOUT_MS)
            if (r < 0) throw IOException(if (closed) "ADB: connection closed" else "USB write failed (cable pulled?)")
            off += r
        }
    }

    override fun readFully(buf: ByteArray, off: Int, len: Int, timeoutMs: Int) {
        var done = 0
        while (done < len) {
            val n = minOf(CHUNK, len - done)
            val r = conn.bulkTransfer(epIn, buf, off + done, n, timeoutMs)
            if (r < 0) {
                if (closed) throw IOException("ADB: connection closed")
                if (timeoutMs > 0) throw AdbTimeoutException("ADB: the board did not answer in $timeoutMs ms")
                throw IOException("USB read failed (cable pulled?)")
            }
            done += r
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        runCatching { conn.releaseInterface(iface) }
        conn.close()
    }

    companion object {
        // Android before 9 caps one bulk transfer at 16 KiB
        private const val CHUNK = 16 * 1024
        private const val WRITE_TIMEOUT_MS = 5000

        private const val ADB_CLASS = 0xff
        private const val ADB_SUBCLASS = 0x42
        private const val ADB_PROTOCOL = 1

        fun adbInterface(device: UsbDevice): UsbInterface? =
            (0 until device.interfaceCount).map { device.getInterface(it) }.firstOrNull {
                it.interfaceClass == ADB_CLASS && it.interfaceSubclass == ADB_SUBCLASS &&
                    it.interfaceProtocol == ADB_PROTOCOL
            }

        fun isAdbDevice(device: UsbDevice) = adbInterface(device) != null

        /** Needs USB permission for [device] already. */
        fun open(manager: UsbManager, device: UsbDevice): UsbAdbChannel {
            val iface = adbInterface(device) ?: throw IOException("${device.deviceName}: no ADB interface")
            var epIn: UsbEndpoint? = null
            var epOut: UsbEndpoint? = null
            for (i in 0 until iface.endpointCount) {
                val e = iface.getEndpoint(i)
                if (e.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (e.direction == UsbConstants.USB_DIR_IN) epIn = e else epOut = e
            }
            if (epIn == null || epOut == null) throw IOException("ADB interface without bulk endpoints")
            val conn = manager.openDevice(device) ?: throw IOException("cannot open USB device (permission?)")
            if (!conn.claimInterface(iface, true)) {
                conn.close()
                throw IOException("cannot claim the ADB interface")
            }
            return UsbAdbChannel(conn, iface, epIn, epOut)
        }
    }
}
