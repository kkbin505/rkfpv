package io.github.kkbin505.rkfpv.fpv

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull

/**
 * Client for the board's `fpv` command (files/fpv): every call is one
 * `fpv <args> --json`, exactly what the PC configurator runs, so anything
 * here can also be typed in `adb shell`. [exec] runs it on the board and
 * returns what it printed: through ADB's shell ([shellCommand]) or
 * dvrhttpd's /api/fpv on the hotspot (see board/).
 */
class FpvClient(private val exec: (List<String>) -> String) {

    fun run(vararg args: String): JsonObject = parseReply(exec(args.toList()))

    fun schema(): List<FpvParam> =
        ok(run("schema"))["params"]!!.jsonArray.map { FpvParam.from(it.jsonObject) }

    fun status(): FpvStatus = FpvStatus(ok(run("status")))

    /** One `fpv set`: the board validates every pair before it writes anything. */
    fun set(values: Map<String, String>): SetResult {
        require(values.isNotEmpty())
        val args = listOf("set") + values.flatMap { listOf(it.key, it.value) }
        val r = ok(run(*args.toTypedArray()))
        return SetResult(
            fpvRunning = r.bool("fpv_running"),
            results = r["results"]!!.jsonArray.map {
                val o = it.jsonObject
                SetResult.Item(o.str("key"), o.str("value"), o.str("apply"), o.str("result"))
            },
            warning = r.str("warning"),
        )
    }

    /** Returns the board's message when FPV is not running (nothing to restart). */
    fun restart(what: String): String? {
        val r = ok(run("restart", what))
        return if (r.bool("restarted")) null else r.str("message")
    }

    fun dvrList(): DvrList {
        val r = ok(run("dvr", "list"))
        return DvrList(
            totalMb = r.str("total_mb").toLongOrNull() ?: 0,
            freeMb = r.str("free_mb").toLongOrNull() ?: 0,
            files = r["files"]!!.jsonArray.map {
                val o = it.jsonObject
                DvrFile(o.str("name"), o["size"]?.jsonPrimitive?.longOrNull ?: 0, o.bool("recording"))
            },
        )
    }

    /** `fpv dvr status`: state, current file and size (needs the video running to record). */
    fun dvrStatus(): DvrStatus {
        val r = ok(run("dvr", "status"))
        return DvrStatus(
            state = r.str("state"), file = r.str("file").takeIf { it != "-" } ?: "",
            bytes = r.str("bytes").toLongOrNull() ?: 0, drops = r.str("drops").toLongOrNull() ?: 0,
            freeMb = r.str("free_mb").toLongOrNull() ?: 0, totalMb = r.str("total_mb").toLongOrNull() ?: 0,
            videoRunning = r.bool("fpv_running"), error = r.str("error"),
        )
    }

    /** Start or stop recording (signals rkfpv); the board refuses when the video is not running. */
    fun dvr(start: Boolean) {
        ok(run("dvr", if (start) "start" else "stop"))
    }

    /**
     * `fpv preview on|off|status` (board on USB, debug mode): runs rkfpv
     * without the radio, so the link stream is served on 127.0.0.1:[PreviewInfo.port].
     */
    fun preview(sub: String): PreviewInfo {
        require(sub in setOf("on", "off", "status"))
        val r = ok(run("preview", sub))
        return PreviewInfo(
            running = r.bool("running"),
            port = (r["port"] as? JsonPrimitive)?.contentOrNull?.toIntOrNull() ?: 5602,
            mode = r.str("mode"),
        )
    }

    fun dvrDelete(names: List<String>): Int {
        require(names.isNotEmpty())
        val r = ok(run("dvr", "delete", *names.toTypedArray()))
        return r["deleted"]?.jsonPrimitive?.contentOrNull?.toIntOrNull() ?: 0
    }

    companion object {
        const val DVR_DIR = "/mnt/dvr"
        private val REC_NAME = Regex("^rec_[0-9]+_[0-9]+\\.(mp4|ts)$")

        fun isRecordingName(name: String) = REC_NAME.matches(name)

        /** The shell line for [args]: `fpv <quoted args> --json`. */
        fun shellCommand(args: List<String>): String = "fpv " + args.joinToString(" ") { quote(it) } + " --json"

        /** POSIX shell single quotes, like Python's shlex.quote. */
        fun quote(s: String): String =
            if (s.isNotEmpty() && s.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it in "_.,:/=+-" }) s
            else "'" + s.replace("'", "'\\''") + "'"

        /** The JSON is the last line starting with '{'; anything before it is stray output. */
        fun parseReply(text: String): JsonObject {
            val t = text.trim()
            for (line in t.lines().asReversed()) {
                val l = line.trim()
                if (!l.startsWith("{")) continue
                val e = runCatching { Json.parseToJsonElement(l) }.getOrNull()
                if (e is JsonObject) return e
                break
            }
            if ("not found" in t) throw FpvException("fpv is not installed on the board", missingFpv = true)
            throw FpvException(t.ifEmpty { "no answer from fpv" }.take(2000))
        }

        private fun ok(r: JsonObject): JsonObject {
            if (r.bool("ok")) return r
            throw FpvException(r.str("error").ifEmpty { "fpv failed" })
        }
    }
}

class FpvException(msg: String, val missingFpv: Boolean = false) : Exception(msg)

internal fun JsonObject.str(k: String): String = (this[k] as? JsonPrimitive)?.contentOrNull ?: ""
internal fun JsonObject.bool(k: String): Boolean = (this[k] as? JsonPrimitive)?.booleanOrNull ?: false

/** One row of `fpv schema`. */
data class FpvParam(
    val key: String,
    /** int | bool | enum */
    val type: String,
    /** words an int also accepts: auto / iq / off */
    val specials: List<String>,
    val choices: List<String>,
    val min: Long,
    val max: Long,
    val default: String,
    /** venc radio fec txpower = live; video link = restart; boot = next boot */
    val apply: String,
    val group: String,
    val help: String,
    val value: String,
) {
    val isLive get() = apply in LIVE
    val isLink get() = apply == "link"

    /** Same rules as validate() in files/fpv; null = valid. */
    fun validate(v: String): String? {
        if (v in specials) return null
        return when (type) {
            "int", "bool" -> {
                val n = if (v.isNotEmpty() && v.all { it in '0'..'9' }) v.toLongOrNull() else null
                when {
                    n == null -> "'$v' is not a number"
                    n < min || n > max -> "$v out of range $min..$max"
                    else -> null
                }
            }
            "enum" -> if (v in choices) null else "'$v' not one of ${choices.joinToString(",")}"
            else -> null
        }
    }

    companion object {
        private val LIVE = setOf("venc", "radio", "fec", "txpower")

        fun from(o: JsonObject): FpvParam {
            fun list(k: String) = (o[k] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList()
            fun num(k: String) = (o[k] as? JsonPrimitive)?.longOrNull ?: 0
            return FpvParam(
                key = o.str("key"), type = o.str("type"), specials = list("specials"),
                choices = list("choices"), min = num("min"), max = num("max"),
                default = o.str("default"), apply = o.str("apply"), group = o.str("group"),
                help = o.str("help"), value = o.str("value"),
            )
        }
    }
}

/** `fpv status`; fields are kept as the board's strings. */
class FpvStatus(val raw: JsonObject) {
    operator fun get(k: String): String = raw.str(k)
    val mode get() = this["mode"]
    val txpowerKey get() = this["txpower_key"]
    val fpvRunning get() = mode == "fpv"
    /** rkfpv runs: FPV mode, or the wired preview in debug mode ("preview") */
    val videoRunning get() = this["rkfpv_pid"].isNotEmpty()
}

data class SetResult(val fpvRunning: Boolean, val results: List<Item>, val warning: String) {
    data class Item(val key: String, val value: String, val apply: String, val result: String)

    /** "video", "link" or "all": what `fpv restart` has to do, null if nothing. */
    fun restartNeeded(): String? {
        val need = results.filter { it.result == "restart-needed" }.map { it.apply }.toSet()
        return when {
            need.isEmpty() -> null
            "video" in need && "link" in need -> "all"
            "link" in need -> "link"
            else -> "video"
        }
    }
}

data class DvrFile(val name: String, val size: Long, val recording: Boolean)

data class DvrStatus(
    /** recording | stopped | no-card | ... (from rkfpv) */
    val state: String,
    val file: String,
    val bytes: Long,
    val drops: Long,
    val freeMb: Long,
    val totalMb: Long,
    val videoRunning: Boolean,
    val error: String,
) {
    val recording get() = state == "recording"
}

data class PreviewInfo(val running: Boolean, val port: Int, val mode: String)

data class DvrList(val totalMb: Long, val freeMb: Long, val files: List<DvrFile>)
