package io.github.kkbin505.rkfpv.board

import io.github.kkbin505.rkfpv.adb.AdbConnection
import io.github.kkbin505.rkfpv.fpv.FpvClient
import java.io.IOException
import java.io.OutputStream

/** The board on USB (debug mode): `fpv` through `shell:`, files through `sync:`. */
class AdbBoard(private val conn: AdbConnection) : Board {
    override val transport get() = Board.Transport.USB
    override val label: String get() = conn.banner
    override val isAlive: Boolean get() = conn.isAlive

    override fun fpv(args: List<String>): String = conn.shell(FpvClient.shellCommand(args))

    /** "tcp:<port>": the service `adb forward` uses, so nothing is forwarded on the phone. */
    override fun openPreview(port: Int): ChunkStream {
        val s = try {
            conn.open("tcp:$port", 3000)
        } catch (e: IOException) {
            // adbd refuses the OPEN while nothing listens on the port
            if (conn.isAlive) throw PreviewNotRunning(e.message ?: "preview not running")
            throw e
        }
        return object : ChunkStream {
            override fun read(): ByteArray? = s.read(0)
            override fun close() = s.close()
        }
    }

    override fun download(name: String, out: OutputStream, onProgress: (Long) -> Unit) {
        conn.sync().use { it.pull("${FpvClient.DVR_DIR}/$name", out, onProgress) }
    }

    override fun close() = conn.close()
}
