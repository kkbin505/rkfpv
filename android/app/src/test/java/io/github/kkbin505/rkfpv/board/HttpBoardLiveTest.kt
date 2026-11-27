package io.github.kkbin505.rkfpv.board

import io.github.kkbin505.rkfpv.fpv.FpvClient
import io.github.kkbin505.rkfpv.fpv.FpvException
import io.github.kkbin505.rkfpv.video.AuReader
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayOutputStream

/**
 * Against a real dvrhttpd (a board in hotspot mode, or dvrhttpd + files/fpv
 * with FPV_ROOT on a PC). Skipped unless RKFPV_TEST_BOARD is set:
 *
 *   RKFPV_TEST_BOARD=127.0.0.1:28080 ./gradlew testDebugUnitTest --tests '*HttpBoardLiveTest*'
 *
 * Changes bitrate and puts it back. Expects recording rec_0003_01.mp4 on the
 * board and frames on the preview port.
 */
class HttpBoardLiveTest {
    private val host: String? = System.getenv("RKFPV_TEST_BOARD")
    private lateinit var board: HttpBoard
    private lateinit var fpv: FpvClient

    @Before fun setUp() {
        assumeTrue("RKFPV_TEST_BOARD not set", !host.isNullOrEmpty())
        board = HttpBoard(host!!)
        fpv = FpvClient { board.fpv(it) }
    }

    @Test fun schemaStatusAndSet() {
        val schema = fpv.schema()
        assertTrue(schema.size > 40)
        assertEquals("hotspot", fpv.status().mode)
        val before = schema.first { it.key == "bitrate" }.value
        val next = if (before == "4000") "4100" else "4000"
        val r = fpv.set(mapOf("bitrate" to next))
        assertEquals(next, r.results.single().value)
        assertEquals(next, fpv.schema().first { it.key == "bitrate" }.value)
        fpv.set(mapOf("bitrate" to before))
        // the board validates too
        try {
            fpv.set(mapOf("bitrate" to "999999"))
            fail()
        } catch (e: FpvException) {
        }
    }

    @Test fun recordings() {
        val list = fpv.dvrList()
        val f = list.files.first { it.name == "rec_0003_01.mp4" }
        val out = ByteArrayOutputStream()
        board.download(f.name, out) {}
        assertEquals(f.size, out.size().toLong())
    }

    @Test fun preview() {
        board.openPreview(5602).use { s ->
            val r = AuReader { s.read() }
            var n = 0
            while (r.read() != null) n++
            assertTrue("frames: $n", n > 0)
        }
    }
}
