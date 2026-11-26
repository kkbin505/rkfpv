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
 * Client for the board's `fpv` command (files/fpv): every call is
 * `fpv <args> --json` through a shell, exactly what the PC configurator
 * runs, so anything here can also be typed in `adb shell`.
 */
class FpvClient(private val shell: (String) -> String) {

    fun run(vararg args: String): JsonObject {
        val cmd = "fpv " + args.joinToString(" ") { quote(it) } + " --json"
        return parseReply(shell(cmd))
    }

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

    fun dvrDelete(names: List<String>): Int {
        require(names.isNotEmpty())
        val r = ok(run("dvr", "delete", *names.toTypedArray()))
        return r["deleted"]?.jsonPrimitive?.contentOrNull?.toIntOrNull() ?: 0
    }

    companion object {
        const val DVR_DIR = "/mnt/dvr"
        private val REC_NAME = Regex("^rec_[0-9]+_[0-9]+\\.(mp4|ts)$")

        fun isRecordingName(name: String) = REC_NAME.matches(name)

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

data class DvrList(val totalMb: Long, val freeMb: Long, val files: List<DvrFile>)
