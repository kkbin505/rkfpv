package io.github.kkbin505.rkfpv.board

import java.io.Closeable
import java.io.IOException
import java.io.OutputStream

/**
 * One way to reach the board. Everything the app does goes through these
 * three calls, so the screens do not care which one is used:
 *
 * - [AdbBoard]: USB cable, the app speaks ADB (board in debug mode)
 * - [HttpBoard]: the board's WiFi hotspot, dvrhttpd at http://10.0.0.1
 *   (board in hotspot mode; the same API the hosted web configurator uses)
 */
interface Board : Closeable {
    val transport: Transport

    /** shown under the status: the adbd banner, or the board's address */
    val label: String

    /** false once the connection is known to be gone */
    val isAlive: Boolean

    /** One `fpv <args> --json`; returns what it printed (the JSON reply). */
    fun fpv(args: List<String>): String

    /**
     * rkfpv's preview stream (u32 big-endian length + H.265 access unit).
     * Throws [PreviewNotRunning] while nothing serves it yet.
     */
    fun openPreview(port: Int): ChunkStream

    /** Copy recording [name] from the DVR directory into [out]; [onProgress] gets the bytes so far. */
    fun download(name: String, out: OutputStream, onProgress: (Long) -> Unit)

    enum class Transport { USB, WIFI }
}

/** A byte stream read in whatever pieces arrive; null at the end. */
interface ChunkStream : Closeable {
    fun read(): ByteArray?
}

/** The preview port does not answer: video not on yet, or restarting. */
class PreviewNotRunning(msg: String) : IOException(msg)
