package io.github.kkbin505.rkfpv.ui

import android.app.Application
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Environment
import android.view.Surface
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.kkbin505.rkfpv.R
import io.github.kkbin505.rkfpv.adb.AdbConnection
import io.github.kkbin505.rkfpv.adb.AdbKey
import io.github.kkbin505.rkfpv.adb.UsbAdbChannel
import io.github.kkbin505.rkfpv.fpv.DvrList
import io.github.kkbin505.rkfpv.fpv.DvrStatus
import io.github.kkbin505.rkfpv.fpv.FpvClient
import io.github.kkbin505.rkfpv.fpv.FpvException
import io.github.kkbin505.rkfpv.fpv.FpvParam
import io.github.kkbin505.rkfpv.fpv.FpvStatus
import io.github.kkbin505.rkfpv.fpv.SetResult
import io.github.kkbin505.rkfpv.video.PreviewPlayer
import io.github.kkbin505.rkfpv.video.PreviewState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream

sealed interface ConnState {
    data object NoBoard : ConnState
    data class NeedPermission(val device: UsbDevice) : ConnState
    data object Connecting : ConnState
    /** The board asked for this phone's key to be accepted. */
    data object AwaitingAuth : ConnState
    data class Connected(val banner: String) : ConnState
    data class Failed(val message: String, val missingFpv: Boolean = false) : ConnState
}

data class LocalRecording(val file: File) {
    val name: String get() = file.name
    val size: Long get() = file.length()
}

data class UiState(
    val conn: ConnState = ConnState.NoBoard,
    val status: FpvStatus? = null,
    val schema: List<FpvParam> = emptyList(),
    val values: Map<String, String> = emptyMap(),
    /** key -> text typed but not saved yet (may be invalid) */
    val edits: Map<String, String> = emptyMap(),
    val saving: Boolean = false,
    val setResult: SetResult? = null,
    val dvr: DvrList? = null,
    val dvrLoading: Boolean = false,
    val local: List<LocalRecording> = emptyList(),
    /** recording name -> bytes copied so far */
    val downloads: Map<String, Long> = emptyMap(),
    /** one-shot text for the snackbar */
    val toast: String? = null,
    val dvrStatus: DvrStatus? = null,
    /** `fpv preview on/off` or `fpv dvr start/stop` in flight */
    val videoBusy: Boolean = false,
    /** null while no preview surface is shown */
    val preview: PreviewState? = null,
) {
    /** rkfpv runs on the board (FPV mode or wired preview) */
    val videoRunning get() = status?.videoRunning == true
    /** DVR is switched on in the settings (`dvr` = 1) */
    val dvrEnabled get() = values["dvr"] == "1"

    val connected get() = conn is ConnState.Connected
    val groups: List<String> get() = schema.map { it.group }.distinct()
    fun param(key: String) = schema.firstOrNull { it.key == key }
    val invalidEdits: List<String>
        get() = edits.filter { (k, v) -> param(k)?.validate(v) != null }.keys.toList()
}

class BoardViewModel(app: Application) : AndroidViewModel(app) {

    private val ctx: Context get() = getApplication()
    private val usb = app.getSystemService(Context.USB_SERVICE) as UsbManager
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var conn: AdbConnection? = null
    private var fpv: FpvClient? = null
    private var device: UsbDevice? = null
    private var pollJob: Job? = null
    /** true from connect() until it succeeded or failed (covers AwaitingAuth too) */
    private var connecting = false
    private val downloadJobs = mutableMapOf<String, Job>()
    private var player: PreviewPlayer? = null
    private var previewPort = 5602

    val dvrDir: File
        get() = File(ctx.getExternalFilesDir(Environment.DIRECTORY_MOVIES) ?: ctx.filesDir, "rkfpv-dvr")

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val dev = usbDevice(i)
            when (i.action) {
                ACTION_USB_PERMISSION -> {
                    if (dev != null && i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) connect(dev)
                    else _state.update { it.copy(conn = ConnState.Failed(ctx.getString(R.string.usb_denied))) }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> scan()
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    if (dev != null && dev.deviceName == device?.deviceName) disconnect(ConnState.NoBoard)
                }
            }
        }
    }

    init {
        val f = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        ContextCompat.registerReceiver(ctx, receiver, f, ContextCompat.RECEIVER_NOT_EXPORTED)
        refreshLocal()
        scan()
    }

    override fun onCleared() {
        player?.let { p -> Thread { p.stop() }.start() }
        ctx.unregisterReceiver(receiver)
        conn?.close()
    }

    // ------------------------------------------------------------- connection

    /** Look for an ADB device on the USB port and connect if we may. */
    fun scan() {
        if (conn?.isAlive == true || connecting) return
        val dev = usb.deviceList.values.firstOrNull { UsbAdbChannel.isAdbDevice(it) }
        when {
            dev == null -> _state.update { it.copy(conn = ConnState.NoBoard) }
            usb.hasPermission(dev) -> connect(dev)
            else -> _state.update { it.copy(conn = ConnState.NeedPermission(dev)) }
        }
    }

    /** From MainActivity when Android launched us for a newly plugged-in board. */
    fun onDeviceAttached(dev: UsbDevice) {
        if (conn?.isAlive == true || connecting || !UsbAdbChannel.isAdbDevice(dev)) return
        if (usb.hasPermission(dev)) connect(dev) else _state.update { it.copy(conn = ConnState.NeedPermission(dev)) }
    }

    fun requestPermission(dev: UsbDevice) {
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val pi = PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_USB_PERMISSION).setPackage(ctx.packageName), flags)
        usb.requestPermission(dev, pi)
    }

    private fun connect(dev: UsbDevice) {
        if (connecting) return
        connecting = true
        device = dev
        _state.update { it.copy(conn = ConnState.Connecting) }
        viewModelScope.launch {
            try {
                val c = withContext(Dispatchers.IO) {
                    val key = AdbKey.loadOrCreate(File(ctx.filesDir, "adbkey"))
                    AdbConnection.connect(
                        UsbAdbChannel.open(usb, dev), key, "rkfpv-android@${Build.MODEL}",
                        onAwaitingAuthorization = { _state.update { it.copy(conn = ConnState.AwaitingAuth) } },
                    )
                }
                conn = c
                fpv = FpvClient { cmd -> c.shell(cmd) }
                loadAll()
                _state.update { it.copy(conn = ConnState.Connected(c.banner)) }
                startPolling()
            } catch (e: CancellationException) {
                throw e
            } catch (e: FpvException) {
                disconnect(ConnState.Failed(e.message ?: "fpv failed", e.missingFpv))
            } catch (e: Exception) {
                disconnect(ConnState.Failed(e.message ?: e.toString()))
            } finally {
                connecting = false
            }
        }
    }

    private fun disconnect(next: ConnState) {
        stopPreview()
        pollJob?.cancel()
        downloadJobs.values.forEach { it.cancel() }
        // closing sends CLSE over USB: not on the main thread
        conn?.let { c -> Thread { c.close() }.start() }
        conn = null
        fpv = null
        _state.update { it.copy(conn = next, status = null, dvr = null, dvrStatus = null, downloads = emptyMap()) }
    }

    fun retry() {
        disconnect(ConnState.NoBoard)
        scan()
    }

    private suspend fun loadAll() = withContext(Dispatchers.IO) {
        val f = fpv ?: return@withContext
        val schema = f.schema()
        val status = f.status()
        val dvr = runCatching { f.dvrList() }.getOrNull()
        val dvrStatus = runCatching { f.dvrStatus() }.getOrNull()
        _state.update { s ->
            s.copy(
                dvrStatus = dvrStatus,
                schema = schema, values = schema.associate { it.key to it.value },
                edits = s.edits.filterKeys { k -> schema.any { it.key == k } },
                status = status, dvr = dvr,
            )
        }
    }

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (isActive) {
                delay(3000)
                val c = conn ?: break
                if (!c.isAlive) {
                    disconnect(ConnState.Failed(ctx.getString(R.string.conn_lost)))
                    break
                }
                runCatching { withContext(Dispatchers.IO) { fpv?.status() } }
                    .onSuccess { st -> if (st != null) _state.update { it.copy(status = st) } }
                if (_state.value.videoRunning || _state.value.dvrStatus?.recording == true) {
                    runCatching { withContext(Dispatchers.IO) { fpv?.dvrStatus() } }
                        .onSuccess { d -> if (d != null) _state.update { it.copy(dvrStatus = d) } }
                }
            }
        }
    }

    /** Run [block] on the board; errors go to the snackbar. */
    private fun board(block: suspend (FpvClient) -> Unit) {
        val f = fpv ?: return
        viewModelScope.launch {
            try {
                withContext(Dispatchers.IO) { block(f) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                toast(e.message ?: e.toString())
                if (conn?.isAlive == false) disconnect(ConnState.Failed(ctx.getString(R.string.conn_lost)))
            }
        }
    }

    fun reload() = board { loadAll() }

    // --------------------------------------------------------------- settings

    fun edit(key: String, value: String) = _state.update { s ->
        val e = s.edits.toMutableMap()
        if (value == s.values[key]) e.remove(key) else e[key] = value
        s.copy(edits = e)
    }

    fun discardEdits() = _state.update { it.copy(edits = emptyMap()) }

    fun save() {
        val s = _state.value
        if (s.edits.isEmpty() || s.invalidEdits.isNotEmpty() || s.saving) return
        val edits = s.edits
        _state.update { it.copy(saving = true) }
        board { f ->
            try {
                val r = f.set(edits)
                _state.update { st ->
                    val v = st.values.toMutableMap()
                    r.results.forEach { v[it.key] = it.value }
                    st.copy(values = v, edits = st.edits - edits.keys, setResult = r)
                }
            } finally {
                _state.update { it.copy(saving = false) }
            }
        }
    }

    fun dismissSetResult() = _state.update { it.copy(setResult = null) }

    fun restart(what: String) {
        dismissSetResult()
        board { f ->
            val msg = f.restart(what)
            toast(msg ?: ctx.getString(R.string.restarted, what))
        }
    }

    // ------------------------------------------------------- video / preview

    /** `fpv preview on|off`: run the video in debug mode (no radio), for the preview and recording. */
    fun setVideo(on: Boolean) {
        if (_state.value.videoBusy) return
        _state.update { it.copy(videoBusy = true) }
        board { f ->
            try {
                val p = f.preview(if (on) "on" else "off")
                previewPort = p.port
                val st = f.status()
                val d = runCatching { f.dvrStatus() }.getOrNull()
                _state.update { it.copy(status = st, dvrStatus = d) }
            } finally {
                _state.update { it.copy(videoBusy = false) }
            }
        }
    }

    /** A preview surface appeared: decode the board's stream onto it. */
    fun startPreview(surface: Surface) {
        val c = conn ?: return
        stopPreview()
        val (w, h) = (_state.value.values["size"] ?: "").split("x").mapNotNull { it.toIntOrNull() }
            .takeIf { it.size == 2 } ?: listOf(1280, 720)
        _state.update { it.copy(preview = PreviewState.Waiting) }
        var self: PreviewPlayer? = null
        val p = PreviewPlayer(c, previewPort, surface, w, h) { ps ->
            // a stopped player may still report once from its thread
            if (player === self) _state.update { it.copy(preview = ps) }
        }
        self = p
        player = p
        p.start()
    }

    /**
     * [wait] = true when the surface is about to go away: the decoder must be
     * released first (stop() joins its thread, normally a few ms).
     */
    fun stopPreview(wait: Boolean = false) {
        val p = player ?: return
        player = null
        if (wait) p.stop() else Thread { p.stop() }.start()
        _state.update { it.copy(preview = null) }
    }

    // -------------------------------------------------------------------- DVR

    fun setRecording(start: Boolean) {
        if (_state.value.videoBusy) return
        _state.update { it.copy(videoBusy = true) }
        board { f ->
            try {
                f.dvr(start)
                // rkfpv starts / closes the file at its next frame
                delay(800)
                val d = f.dvrStatus()
                _state.update { it.copy(dvrStatus = d) }
                if (!start) _state.update { it.copy(dvr = f.dvrList()) }
            } finally {
                _state.update { it.copy(videoBusy = false) }
            }
        }
    }

    fun refreshDvr() {
        refreshLocal()
        if (fpv == null) return
        _state.update { it.copy(dvrLoading = true) }
        board { f ->
            try {
                val d = f.dvrList()
                _state.update { it.copy(dvr = d) }
            } finally {
                _state.update { it.copy(dvrLoading = false) }
            }
        }
    }

    fun refreshLocal() {
        val files = dvrDir.listFiles { f -> f.isFile && FpvClient.isRecordingName(f.name) }
            ?.sortedByDescending { it.name }?.map { LocalRecording(it) } ?: emptyList()
        _state.update { it.copy(local = files) }
    }

    fun download(name: String) {
        val c = conn ?: return
        if (!FpvClient.isRecordingName(name) || name in downloadJobs) return
        _state.update { it.copy(downloads = it.downloads + (name to 0L)) }
        downloadJobs[name] = viewModelScope.launch {
            val dir = dvrDir.apply { mkdirs() }
            val part = File(dir, "$name.part")
            try {
                withContext(Dispatchers.IO) {
                    c.sync().use { sync ->
                        FileOutputStream(part).use { out ->
                            var last = 0L
                            sync.pull("${FpvClient.DVR_DIR}/$name", out) { n ->
                                if (!isActive) throw CancellationException()
                                if (n - last >= 256 * 1024) {
                                    last = n
                                    _state.update { it.copy(downloads = it.downloads + (name to n)) }
                                }
                            }
                        }
                    }
                    val dest = File(dir, name)
                    dest.delete()
                    if (!part.renameTo(dest)) throw java.io.IOException("cannot save $name")
                }
                toast(ctx.getString(R.string.downloaded, name))
            } catch (e: CancellationException) {
                part.delete()
            } catch (e: Exception) {
                part.delete()
                toast(e.message ?: e.toString())
            } finally {
                downloadJobs.remove(name)
                _state.update { it.copy(downloads = it.downloads - name) }
                refreshLocal()
            }
        }
    }

    fun cancelDownload(name: String) {
        downloadJobs[name]?.cancel()
    }

    fun deleteOnBoard(name: String) = board { f ->
        f.dvrDelete(listOf(name))
        _state.update { it.copy(dvr = f.dvrList()) }
    }

    fun deleteLocal(rec: LocalRecording) {
        rec.file.delete()
        refreshLocal()
    }

    // ------------------------------------------------------------------ misc

    fun toast(msg: String) = _state.update { it.copy(toast = msg) }
    fun toastShown() = _state.update { it.copy(toast = null) }

    companion object {
        const val ACTION_USB_PERMISSION = "io.github.kkbin505.rkfpv.USB_PERMISSION"

        @Suppress("DEPRECATION")
        fun usbDevice(i: Intent): UsbDevice? =
            if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
    }
}
