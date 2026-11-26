package io.github.kkbin505.rkfpv.video

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import io.github.kkbin505.rkfpv.adb.AdbConnection
import io.github.kkbin505.rkfpv.adb.AdbStream
import java.io.IOException
import java.nio.ByteBuffer

sealed interface PreviewState {
    /** waiting for rkfpv to listen (video starting / restarting) */
    data object Waiting : PreviewState
    data class Playing(val width: Int, val height: Int, val fps: Float, val dropped: Long) : PreviewState
    data class Failed(val message: String) : PreviewState
}

/**
 * Wired preview: reads rkfpv's preview port through ADB ("tcp:<port>",
 * the service `adb forward` uses, so nothing is forwarded on the phone) and
 * decodes it with MediaCodec onto [surface].
 *
 * Latency first: every decoded frame is shown at once; when the decoder
 * falls behind, frames are dropped up to the next key frame. When the
 * stream ends (rkfpv restarted for a setting) it reconnects.
 */
class PreviewPlayer(
    private val conn: AdbConnection,
    private val port: Int,
    private val surface: Surface,
    /** size for the decoder before the SPS is seen, e.g. from the `size` setting */
    private val hintWidth: Int,
    private val hintHeight: Int,
    private val onState: (PreviewState) -> Unit,
) {
    @Volatile private var running = false
    @Volatile private var stream: AdbStream? = null
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread(::loop, "preview").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        stream?.close()
        thread?.join(2000)
        thread = null
    }

    private fun loop() {
        var failures = 0
        while (running) {
            val s = try {
                conn.open("tcp:$port", 3000)
            } catch (e: IOException) {
                // nothing listens yet: adbd refuses the OPEN
                if (!conn.isAlive) { onState(PreviewState.Failed(e.message ?: "ADB closed")); return }
                onState(PreviewState.Waiting)
                sleep(400)
                continue
            }
            stream = s
            try {
                decode(s)
                failures = 0
            } catch (e: Exception) {
                if (!running) break
                if (++failures >= 5) {
                    onState(PreviewState.Failed(e.message ?: e.toString()))
                    sleep(2000)
                }
            } finally {
                s.close()
                stream = null
            }
            if (running) {
                onState(PreviewState.Waiting)
                sleep(300)
            }
        }
    }

    private fun decode(s: AdbStream) {
        val reader = AuReader { if (running) s.read(0) else null }
        var codec: MediaCodec? = null
        val info = MediaCodec.BufferInfo()
        var pts = 0L
        var needKey = false
        var dropped = 0L
        var width = hintWidth
        var height = hintHeight
        var frames = 0
        var fps = 0f
        var t0 = System.nanoTime()

        fun report() = onState(PreviewState.Playing(width, height, fps, dropped))

        fun drain(c: MediaCodec) {
            while (true) {
                val o = c.dequeueOutputBuffer(info, 0)
                when {
                    o >= 0 -> {
                        c.releaseOutputBuffer(o, true)
                        frames++
                        val now = System.nanoTime()
                        if (now - t0 >= 1_000_000_000L) {
                            fps = frames * 1e9f / (now - t0)
                            frames = 0
                            t0 = now
                            report()
                        }
                    }
                    o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val f = c.outputFormat
                        width = f.getInteger(MediaFormat.KEY_WIDTH)
                        height = f.getInteger(MediaFormat.KEY_HEIGHT)
                        if (f.containsKey("crop-right") && f.containsKey("crop-left")) {
                            width = f.getInteger("crop-right") - f.getInteger("crop-left") + 1
                            height = f.getInteger("crop-bottom") - f.getInteger("crop-top") + 1
                        }
                        report()
                    }
                    else -> return
                }
            }
        }

        try {
            while (running) {
                val au = reader.read() ?: return
                var c = codec
                if (c == null) {
                    if (!H265.isKeyFrame(au)) continue
                    val csd = H265.parameterSets(au)
                    if (csd.isEmpty()) continue
                    val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_HEVC, hintWidth, hintHeight)
                    fmt.setByteBuffer("csd-0", ByteBuffer.wrap(csd))
                    fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, AuReader.MAX_AU)
                    if (Build.VERSION.SDK_INT >= 30) fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                    c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC)
                    c.configure(fmt, surface, null, 0)
                    c.start()
                    codec = c
                    report()
                }
                if (needKey && !H265.isKeyFrame(au)) {
                    dropped++
                    drain(c)
                    continue
                }
                val idx = c.dequeueInputBuffer(20_000)
                if (idx < 0) {
                    // decoder is behind: skip to the next key frame
                    needKey = true
                    dropped++
                    drain(c)
                    continue
                }
                val buf = c.getInputBuffer(idx)!!
                buf.clear()
                if (au.size > buf.capacity()) {
                    c.queueInputBuffer(idx, 0, 0, pts, 0)
                    needKey = true
                    dropped++
                } else {
                    buf.put(au)
                    c.queueInputBuffer(idx, 0, au.size, pts, 0)
                    needKey = false
                }
                pts += 33_333
                drain(c)
            }
        } finally {
            codec?.let { runCatching { it.stop() }; runCatching { it.release() } }
        }
    }

    private fun sleep(ms: Long) {
        try {
            Thread.sleep(ms)
        } catch (_: InterruptedException) {
        }
    }
}
