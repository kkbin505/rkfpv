package io.github.kkbin505.rkfpv.video

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.IOException

class H265Test {

    /** NAL header byte for a type: forbidden 0, type in bits 6..1, layer id 0 */
    private fun nal(type: Int, vararg body: Int) = byteArrayOf((type shl 1).toByte(), 1) + body.map { it.toByte() }

    private fun annexB(vararg nals: ByteArray, long: Boolean = true): ByteArray {
        val o = ByteArrayOutputStream()
        nals.forEachIndexed { i, n ->
            o.write(if (long || i == 0) byteArrayOf(0, 0, 0, 1) else byteArrayOf(0, 0, 1))
            o.write(n)
        }
        return o.toByteArray()
    }

    private val vps = nal(32, 0x0c, 0x01)
    // a real NAL never ends in 0x00 (rbsp trailing bits), so a zero before 00 00 01 belongs to the start code
    private val sps = nal(33, 0x01, 0x60, 0x80)
    private val pps = nal(34, 0xc1, 0x72)
    private val idr = nal(19, 0xaf, 0x00, 0x00, 0x03, 0x01)   // contains 00 00 03 (emulation prevention)
    private val trail = nal(1, 0x22, 0x33)

    @Test fun keyFrameAndParameterSets() {
        val key = annexB(vps, sps, pps, idr)
        assertEquals(listOf(32, 33, 34, 19), H265.nals(key).map { it.type })
        assertTrue(H265.isKeyFrame(key))
        assertArrayEquals(annexB(vps, sps, pps), H265.parameterSets(key))
    }

    @Test fun shortStartCodes() {
        val key = annexB(vps, sps, pps, idr, long = false)
        assertEquals(listOf(32, 33, 34, 19), H265.nals(key).map { it.type })
        // csd-0 always gets 4-byte start codes
        assertArrayEquals(annexB(vps, sps, pps), H265.parameterSets(key))
    }

    @Test fun deltaFrame() {
        val p = annexB(trail)
        assertFalse(H265.isKeyFrame(p))
        assertEquals(0, H265.parameterSets(p).size)
        // a key frame without VPS/SPS/PPS cannot start a decoder
        assertEquals(0, H265.parameterSets(annexB(idr)).size)
        assertTrue(H265.isKeyFrame(annexB(nal(21, 0x01))))   // CRA
    }

    private fun framed(vararg aus: ByteArray): ByteArray {
        val o = ByteArrayOutputStream()
        for (a in aus) {
            o.write(byteArrayOf((a.size ushr 24).toByte(), (a.size ushr 16).toByte(), (a.size ushr 8).toByte(), a.size.toByte()))
            o.write(a)
        }
        return o.toByteArray()
    }

    /** Feeds [bytes] in chunks of [chunk] bytes, then end of stream. */
    private fun reader(bytes: ByteArray, chunk: Int): AuReader {
        var pos = 0
        return AuReader {
            if (pos >= bytes.size) null
            else bytes.copyOfRange(pos, minOf(bytes.size, pos + chunk)).also { pos += it.size }
        }
    }

    @Test fun framingAcrossAnyChunking() {
        val a = annexB(vps, sps, pps, idr)
        val b = annexB(trail)
        val c = ByteArray(70_000) { (it % 251).toByte() }
        val stream = framed(a, b, c)
        for (chunk in listOf(1, 3, 4, 5, 1000, 65536, stream.size)) {
            val r = reader(stream, chunk)
            assertArrayEquals(a, r.read())
            assertArrayEquals(b, r.read())
            assertArrayEquals(c, r.read())
            assertNull(r.read())
        }
    }

    @Test fun streamEndsInsideFrame() {
        val s = framed(annexB(trail))
        try {
            reader(s.copyOf(s.size - 1), 4).read()
            fail()
        } catch (e: EOFException) {
            // expected
        }
    }

    @Test fun badLength() {
        try {
            reader(byteArrayOf(0x7f, 0, 0, 0, 1), 8).read()
            fail()
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("bad frame length"))
        }
    }
}
