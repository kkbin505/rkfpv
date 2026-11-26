package io.github.kkbin505.rkfpv.fpv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** Fixtures in resources/fpv were captured from files/fpv run with FPV_ROOT on a PC. */
class FpvClientTest {

    private fun fixture(name: String) = javaClass.classLoader!!.getResource("fpv/$name")!!.readText()

    /** A client whose "board" answers [reply] and records the command line. */
    private fun client(reply: String, sent: MutableList<String> = mutableListOf()) =
        FpvClient { cmd -> sent += cmd; reply }

    @Test fun schemaParses() {
        val params = client(fixture("schema.json")).schema()
        val table = fixture("schema.json")
        assertTrue(params.size > 40)
        assertEquals(params.size, Regex("\"key\":").findAll(table).count())
        val fps = params.first { it.key == "fps" }
        assertEquals("int", fps.type); assertEquals(1, fps.min); assertEquals(60, fps.max)
        assertEquals("video", fps.apply); assertEquals("Video", fps.group)
        assertEquals(listOf("auto"), params.first { it.key == "txpower_8812eu" }.specials)
        assertEquals(listOf("iq", "off"), params.first { it.key == "isp_nr_spatial" }.specials)
        assertEquals(listOf("cbr", "vbr"), params.first { it.key == "rc" }.choices)
        assertTrue(params.first { it.key == "channel" }.isLink)
        assertTrue(params.first { it.key == "bitrate" }.isLive)
        assertEquals("Video", params.first().group)
    }

    /** Every case was checked against `fpv set` itself (validate_cases.txt). */
    @Test fun validateMatchesTheBoardScript() {
        val params = client(fixture("schema.json")).schema().associateBy { it.key }
        val cases = fixture("validate_cases.txt").lines().filter { it.isNotBlank() && !it.startsWith("#") }
        assertTrue(cases.size > 30)
        for (c in cases) {
            val (k, v, ok) = c.split("|")
            val err = params.getValue(k).validate(v)
            assertEquals("$k=$v", ok == "1", err == null)
        }
    }

    @Test fun statusParses() {
        val st = client(fixture("status.json")).status()
        assertEquals("idle", st.mode)
        assertEquals("12345", st["mem_avail_kb"])
        assertFalse(st.fpvRunning)
    }

    @Test fun setSendsOneCommandAndParses() {
        val sent = mutableListOf<String>()
        val r = client(fixture("set.json"), sent).set(linkedMapOf("bitrate" to "4000", "isp_hue" to "iq"))
        assertEquals(listOf("fpv set bitrate 4000 isp_hue iq --json"), sent)
        assertEquals(2, r.results.size)
        assertEquals("saved", r.results[0].result)
        assertFalse(r.fpvRunning)
        assertNull(r.restartNeeded())
    }

    @Test fun setErrorFromBoard() {
        try {
            client(fixture("set_bad.json")).set(mapOf("bitrate" to "99"))
            fail("expected error")
        } catch (e: FpvException) {
            assertEquals("bitrate: 99 out of range 100..50000", e.message)
        }
    }

    @Test fun restartNeeded() {
        fun r(vararg a: Pair<String, String>) =
            SetResult(true, a.map { SetResult.Item("k", "v", it.first, it.second) }, "")
        assertEquals("video", r("video" to "restart-needed", "venc" to "live").restartNeeded())
        assertEquals("link", r("link" to "restart-needed").restartNeeded())
        assertEquals("all", r("video" to "restart-needed", "link" to "restart-needed").restartNeeded())
        assertNull(r("venc" to "live", "boot" to "next-boot").restartNeeded())
    }

    @Test fun dvrList() {
        val d = client(fixture("dvr_list.json")).dvrList()
        assertEquals(listOf("rec_0003_02.mp4", "rec_0003_01.mp4"), d.files.map { it.name })
        assertEquals(1048577L, d.files[0].size)
    }

    @Test fun replyWithStrayOutputAndCrLf() {
        val r = FpvClient.parseReply("some warning\r\n{\"ok\":true,\"x\":\"1\"}\r\n")
        assertEquals("1", r.str("x"))
    }

    @Test fun missingFpv() {
        try {
            FpvClient.parseReply("/bin/sh: fpv: not found\n")
            fail()
        } catch (e: FpvException) {
            assertTrue(e.missingFpv)
        }
    }

    @Test fun quoting() {
        assertEquals("bitrate", FpvClient.quote("bitrate"))
        assertEquals("1280x720", FpvClient.quote("1280x720"))
        assertEquals("'a b'", FpvClient.quote("a b"))
        assertEquals("'it'\\''s'", FpvClient.quote("it's"))
        assertEquals("'\$(reboot)'", FpvClient.quote("\$(reboot)"))
        assertEquals("''", FpvClient.quote(""))
    }

    @Test fun recordingNames() {
        assertTrue(FpvClient.isRecordingName("rec_0003_01.mp4"))
        assertTrue(FpvClient.isRecordingName("rec_12_3.ts"))
        assertFalse(FpvClient.isRecordingName("../etc/passwd"))
        assertFalse(FpvClient.isRecordingName("rec_1_1.mp4.part"))
    }
}
