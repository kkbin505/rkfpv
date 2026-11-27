package io.github.kkbin505.rkfpv.ui

import android.app.Application
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
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
import io.github.kkbin505.rkfpv.board.AdbBoard
import io.github.kkbin505.rkfpv.board.Board
import io.github.kkbin505.rkfpv.board.HttpBoard
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
import java.net.DatagramSocket
import java.net.HttpURLConnection

sealed interface ConnState {
    data object NoBoard : ConnState
    data class NeedPermission(val device: UsbDevice) : ConnState
    data class Connecting(val transport: Board.Transport) : ConnState
    /** The board asked for this phone's key to be accepted. */
    data object AwaitingAuth : ConnState
    data class Connected(val banner: String, val transport: Board.Transport) : ConnState
    data class Failed(
        val message: String,
        val missingFpv: Boolean = false,
        val transport: Board.Transport = Board.Transport.USB,
    ) : ConnState
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
    /** the board's address on its hotspot */
    val wifiHost: String = HttpBoard.DEFAULT_HOST,
) {
    /** rkfpv runs on the board (FPV mode or wired preview) */
    val videoRunning get() = status?.videoRunning == true
    /** DVR is switched on in the settings (`dvr` = 1) */
    val dvrEnabled get() = values["dvr"] == "1"

    val connected get() = conn is ConnState.Connected
    val overWifi get() = (conn as? ConnState.Connected)?.transport == Board.Transport.WIFI
    /**
     * hotspot mode: the WiFi card is the access point, so no link and no
     * recording. Older board images report "idle" there; over WiFi it can
     * only be the hotspot.
     */
    val hotspotMode get() = status?.mode == "hotspot" || (overWifi && status?.fpvRunning == false)
    val groups: List<String> get() = schema.map { it.group }.distinct()
    fun param(key: String) = schema.firstOrNull { it.key == key }
    val invalidEdits: List<String>
        get() = edits.filter { (k, v) -> param(k)?.validate(v) != null }.keys.toList()
}

class BoardViewModel(app: Application) : AndroidViewModel(app) {

    private val ctx: Context get() = getApplication()
    private val usb = app.getSystemService(Context.USB_SERVICE) as UsbManager
    private val cm = app.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val prefs = app.getSharedPreferences("board", Context.MODE_PRIVATE)
    private val _state = MutableStateFlow(UiState(wifiHost = prefs.getString(PREF_HOST, null) ?: HttpBoard.DEFAULT_HOST))
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var board: Board? = null
    private var fpv: FpvClient? = null
    /** the USB device [board] runs on (USB only) */
    private var device: UsbDevice? = null
    /** the WiFi network the phone is on (the board's hotspot, hopefully) */
    @Volatile private var wifiNetwork: Network? = null
    private var pollJob: Job? = null
    /** true from connect() until it succeeded or failed (covers AwaitingAuth too) */
    private var connecting = false
    private val downloadJobs = mutableMapOf<String, Job>()
    private var player: PreviewPlayer? = null
    private var playerSurface: Surface? = null
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
                    if (dev != null && board is AdbBoard && dev.deviceName == device?.deviceName) {
                        disconnect(ConnState.NoBoard)
                    }
                }
            }
        }
    }

    /**
     * Tracks the WiFi network. The hotspot has no internet, so Android keeps
     * mobile data as the default route: requests to the board must be made
     * on this Network explicitly (HttpBoard's opener).
     */
    private val wifiCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(n: Network) {
            wifiNetwork = n
            // back on a WiFi (the hotspot came back?): reconnect if that is how we were connected
            viewModelScope.launch {
                if (board == null && !connecting && prefs.getString(PREF_LAST, null) == Board.Transport.WIFI.name) {
                    connectWifi(quiet = true)
                }
            }
        }

        override fun onLost(n: Network) {
            if (wifiNetwork != n) return
            wifiNetwork = null
            viewModelScope.launch {
                if (board is HttpBoard) {
                    disconnect(ConnState.Failed(ctx.getString(R.string.wifi_lost), transport = Board.Transport.WIFI))
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
        val wifi = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            // the hotspot has no internet: without this it would never match
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        runCatching { cm.registerNetworkCallback(wifi, wifiCallback) }
        refreshLocal()
        scan()
    }

    override fun onCleared() {
        player?.let { p -> Thread { p.stop() }.start() }
        ctx.unregisterReceiver(receiver)
        runCatching { cm.unregisterNetworkCallback(wifiCallback) }
        board?.let { b -> Thread { b.close() }.start() }
    }

    // ------------------------------------------------------------- connection

    /**
     * Look for an ADB device on the USB port and connect if we may. With
     * none, reconnect over WiFi if that is how we were connected last time
     * and the phone is on a WiFi network (quietly: no error if it fails).
     */
    fun scan() {
        if (board?.isAlive == true || connecting) return
        val dev = usb.deviceList.values.firstOrNull { UsbAdbChannel.isAdbDevice(it) }
        when {
            dev != null && usb.hasPermission(dev) -> connect(dev)
            dev != null -> _state.update { it.copy(conn = ConnState.NeedPermission(dev)) }
            // a failed WiFi attempt stays on screen until Retry
            (_state.value.conn as? ConnState.Failed)?.transport == Board.Transport.WIFI -> {}
            prefs.getString(PREF_LAST, null) == Board.Transport.WIFI.name && wifiNetwork != null ->
                connectWifi(quiet = true)
            else -> _state.update { it.copy(conn = ConnState.NoBoard) }
        }
    }

    /** From MainActivity when Android launched us for a newly plugged-in board. */
    fun onDeviceAttached(dev: UsbDevice) {
        if (board?.isAlive == true || connecting || !UsbAdbChannel.isAdbDevice(dev)) return
        if (usb.hasPermission(dev)) connect(dev) else _state.update { it.copy(conn = ConnState.NeedPermission(dev)) }
    }

    fun setWifiHost(h: String) = _state.update { it.copy(wifiHost = h) }

    fun requestPermission(dev: UsbDevice) {
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val pi = PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_USB_PERMISSION).setPackage(ctx.packageName), flags)
        usb.requestPermission(dev, pi)
    }

    private fun connect(dev: UsbDevice) {
        if (connecting) return
        device = dev
        attach(Board.Transport.USB, quiet = false) {
            val key = AdbKey.loadOrCreate(File(ctx.filesDir, "adbkey"))
            AdbBoard(
                AdbConnection.connect(
                    UsbAdbChannel.open(usb, dev), key, "rkfpv-android@${Build.MODEL}",
                    onAwaitingAuthorization = { _state.update { it.copy(conn = ConnState.AwaitingAuth) } },
                ),
            )
        }
    }

    /**
     * The board's hotspot (hotspot mode: BOOT held 2 s); the phone has to be
     * on its WiFi already. [quiet]: an automatic attempt, back to "no board"
     * instead of an error when nothing answers.
     */
    fun connectWifi(quiet: Boolean = false) {
        if (board?.isAlive == true) return
        val host = _state.value.wifiHost.trim().ifEmpty { HttpBoard.DEFAULT_HOST }
        attach(Board.Transport.WIFI, quiet) {
            val net = wifiNetwork
            HttpBoard(
                host,
                open = { url -> (net?.openConnection(url) ?: url.openConnection()) as HttpURLConnection },
                datagramSocket = { DatagramSocket().also { s -> net?.bindSocket(s) } },
                // no WiFi network seen (an emulator, say): the default route
                networkUp = { net == null || wifiNetwork == net },
            )
        }
    }

    /** Open a board with [make], read everything once, then poll. */
    private fun attach(transport: Board.Transport, quiet: Boolean, make: () -> Board) {
        if (connecting) return
        connecting = true
        _state.update { it.copy(conn = ConnState.Connecting(transport)) }
        viewModelScope.launch {
            try {
                val b = withContext(Dispatchers.IO) { make() }
                board = b
                fpv = FpvClient { args -> b.fpv(args) }
                loadAll()
                _state.update { it.copy(conn = ConnState.Connected(b.label, transport)) }
                val e = prefs.edit().putString(PREF_LAST, transport.name)
                if (transport == Board.Transport.WIFI) e.putString(PREF_HOST, _state.value.wifiHost.trim())
                e.apply()
                startPolling()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                val next = when {
                    quiet -> ConnState.NoBoard
                    e is FpvException -> ConnState.Failed(e.message ?: "fpv failed", e.missingFpv, transport)
                    transport == Board.Transport.WIFI -> ConnState.Failed(
                        ctx.getString(R.string.wifi_failed, _state.value.wifiHost.trim(), e.message ?: e.toString()),
                        transport = transport,
                    )
                    else -> ConnState.Failed(e.message ?: e.toString(), transport = transport)
                }
                disconnect(next)
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
        board?.let { b -> Thread { b.close() }.start() }
        board = null
        fpv = null
        device = null
        _state.update { it.copy(conn = next, status = null, dvr = null, dvrStatus = null, downloads = emptyMap()) }
    }

    /** Disconnect on purpose: stay disconnected, also at the next start. */
    fun disconnectByUser() {
        prefs.edit().remove(PREF_LAST).apply()
        disconnect(ConnState.NoBoard)
    }

    fun retry() {
        val c = _state.value.conn
        val wifi = (c as? ConnState.Failed)?.transport == Board.Transport.WIFI ||
            (c as? ConnState.Connected)?.transport == Board.Transport.WIFI
        disconnect(ConnState.NoBoard)
        if (wifi && usb.deviceList.values.none { UsbAdbChannel.isAdbDevice(it) }) connectWifi() else scan()
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
            // HTTP has no connection to watch: lost is when the board stops answering
            var misses = 0
            while (isActive) {
                delay(3000)
                val b = board ?: break
                if (!b.isAlive || misses >= MAX_MISSES) {
                    disconnect(ConnState.Failed(ctx.getString(R.string.conn_lost), transport = b.transport))
                    break
                }
                runCatching { withContext(Dispatchers.IO) { fpv?.status() } }
                    .onSuccess { st -> misses = 0; if (st != null) _state.update { it.copy(status = st) } }
                    .onFailure { e -> if (e is CancellationException) throw e; if (e !is FpvException) misses++ }
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
                board?.let { b ->
                    if (!b.isAlive) disconnect(ConnState.Failed(ctx.getString(R.string.conn_lost), transport = b.transport))
                }
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
        val b = board ?: return
        // a player on another surface (switching to / from full screen): release
        // its decoder before that surface goes away
        stopPreview(wait = true)
        val (w, h) = (_state.value.values["size"] ?: "").split("x").mapNotNull { it.toIntOrNull() }
            .takeIf { it.size == 2 } ?: listOf(1280, 720)
        _state.update { it.copy(preview = PreviewState.Waiting) }
        var self: PreviewPlayer? = null
        val p = PreviewPlayer(b, previewPort, surface, w, h) { ps ->
            // a stopped player may still report once from its thread
            if (player === self) _state.update { it.copy(preview = ps) }
        }
        self = p
        player = p
        playerSurface = surface
        p.start()
    }

    /**
     * [wait] = true when the surface is about to go away: the decoder must be
     * released first (stop() joins its thread, normally a few ms).
     */
    fun stopPreview(wait: Boolean = false, surface: Surface? = null) {
        // [surface] going away is not the one playing: the new view's surface came first
        if (surface != null && surface !== playerSurface) return
        val p = player ?: return
        player = null
        playerSurface = null
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
        val b = board ?: return
        if (!FpvClient.isRecordingName(name) || name in downloadJobs) return
        _state.update { it.copy(downloads = it.downloads + (name to 0L)) }
        downloadJobs[name] = viewModelScope.launch {
            val dir = dvrDir.apply { mkdirs() }
            val part = File(dir, "$name.part")
            try {
                withContext(Dispatchers.IO) {
                    FileOutputStream(part).use { out ->
                        var last = 0L
                        b.download(name, out) { n ->
                            if (!isActive) throw CancellationException()
                            if (n - last >= 256 * 1024) {
                                last = n
                                _state.update { it.copy(downloads = it.downloads + (name to n)) }
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
        private const val PREF_HOST = "wifi_host"
        /** transport of the last successful connection */
        private const val PREF_LAST = "last_transport"
        /** status polls (3 s apart) that may go unanswered over WiFi before we call it lost */
        private const val MAX_MISSES = 3

        @Suppress("DEPRECATION")
        fun usbDevice(i: Intent): UsbDevice? =
            if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
    }
}
