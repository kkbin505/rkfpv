package io.github.kkbin505.rkfpv.video

import java.io.EOFException
import java.io.IOException

/** Just enough H.265 Annex-B parsing to start a decoder at a key frame. */
object H265 {
    const val NAL_IDR_W_RADL = 19
    const val NAL_IDR_N_LP = 20
    const val NAL_CRA = 21
    const val NAL_VPS = 32
    const val NAL_SPS = 33
    const val NAL_PPS = 34

    /** A NAL unit inside an access unit: [start] is the first byte after the start code. */
    class Nal(val type: Int, val start: Int, val end: Int, val startCodeLen: Int)

    fun nals(au: ByteArray): List<Nal> {
        val out = ArrayList<Nal>()
        var i = 0
        var cur = -1
        var curSc = 0
        while (i + 2 < au.size) {
            if (au[i].toInt() == 0 && au[i + 1].toInt() == 0 && au[i + 2].toInt() == 1) {
                // 00 00 01, or 00 00 00 01 when the byte before is 0 too
                val sc = if (i > 0 && au[i - 1].toInt() == 0) 4 else 3
                val scStart = i + 3 - sc
                if (cur >= 0) out += nal(au, cur, scStart, curSc)
                cur = i + 3
                curSc = sc
                i += 3
            } else {
                i++
            }
        }
        if (cur >= 0 && cur < au.size) out += nal(au, cur, au.size, curSc)
        return out
    }

    private fun nal(au: ByteArray, start: Int, end: Int, sc: Int) =
        Nal(if (start < end) (au[start].toInt() shr 1) and 0x3f else -1, start, end, sc)

    /** IDR or CRA: decoding can start here. */
    fun isKeyFrame(au: ByteArray): Boolean =
        nals(au).any { it.type == NAL_IDR_W_RADL || it.type == NAL_IDR_N_LP || it.type == NAL_CRA }

    /** VPS + SPS + PPS with 4-byte start codes (MediaCodec "csd-0"), empty if missing. */
    fun parameterSets(au: ByteArray): ByteArray {
        val ps = nals(au).filter { it.type == NAL_VPS || it.type == NAL_SPS || it.type == NAL_PPS }
        if (ps.map { it.type }.toSet().size < 3) return ByteArray(0)
        val out = java.io.ByteArrayOutputStream()
        for (n in ps) {
            out.write(byteArrayOf(0, 0, 0, 1))
            out.write(au, n.start, n.end - n.start)
        }
        return out.toByteArray()
    }
}

/**
 * The preview stream of rkfpv (src/preview.c): each access unit is a
 * u32 big-endian length followed by that many bytes of Annex-B H.265.
 * [next] supplies the raw bytes in chunks of any size; null = stream end.
 */
class AuReader(private val next: () -> ByteArray?) {
    private var buf = ByteArray(0)
    private var pos = 0

    /** The next access unit, or null when the stream ended between units. */
    fun read(): ByteArray? {
        val head = readExact(4, eofOk = true) ?: return null
        val len = ((head[0].toInt() and 0xff) shl 24) or ((head[1].toInt() and 0xff) shl 16) or
            ((head[2].toInt() and 0xff) shl 8) or (head[3].toInt() and 0xff)
        if (len <= 0 || len > MAX_AU) throw IOException("preview: bad frame length $len")
        return readExact(len, eofOk = false)
    }

    private fun readExact(n: Int, eofOk: Boolean): ByteArray? {
        val out = ByteArray(n)
        var done = 0
        while (done < n) {
            if (pos >= buf.size) {
                val b = next()
                if (b == null) {
                    if (eofOk && done == 0) return null
                    throw EOFException("preview: stream ended inside a frame")
                }
                buf = b
                pos = 0
                continue
            }
            val k = minOf(n - done, buf.size - pos)
            System.arraycopy(buf, pos, out, done, k)
            pos += k
            done += k
        }
        return out
    }

    companion object {
        /** rkfpv drops larger units (PREVIEW_MAX_AU) */
        const val MAX_AU = 1024 * 1024
    }
}
