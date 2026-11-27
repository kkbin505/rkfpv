package io.github.kkbin505.rkfpv.ui

import android.app.Activity
import android.content.Intent
import android.content.res.Configuration
import android.hardware.usb.UsbManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.kkbin505.rkfpv.R

class MainActivity : ComponentActivity() {
    private val vm: BoardViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        handleUsbIntent(intent)
        setContent { RkfpvTheme { RkfpvApp(vm) } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleUsbIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        vm.scan()
    }

    private fun handleUsbIntent(i: Intent?) {
        if (i?.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            BoardViewModel.usbDevice(i)?.let { vm.onDeviceAttached(it) }
        }
    }
}

@Composable
fun RkfpvTheme(content: @Composable () -> Unit) {
    val accent = Color(0xFFFFA000)
    val scheme = if (isSystemInDarkTheme()) darkColorScheme(primary = accent, secondary = accent)
    else lightColorScheme(primary = Color(0xFFE65100), secondary = Color(0xFFE65100))
    MaterialTheme(colorScheme = scheme, content = content)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RkfpvApp(vm: BoardViewModel) {
    val s by vm.state.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableIntStateOf(0) }
    val snackbar = remember { SnackbarHostState() }

    // full screen preview: Preview tab, picture running, phone sideways (turned, or forced by the button)
    val activity = LocalContext.current as Activity
    val rotation = remember { Rotation(activity) }
    DisposableEffect(Unit) { onDispose { rotation.release() } }
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val fullscreen = tab == 1 && s.connected && s.videoRunning && landscape
    val view = LocalView.current
    DisposableEffect(fullscreen) {
        val c = WindowCompat.getInsetsController(activity.window, view)
        if (fullscreen) {
            c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            c.hide(WindowInsetsCompat.Type.systemBars())
        } else {
            c.show(WindowInsetsCompat.Type.systemBars())
        }
        onDispose { }
    }

    LaunchedEffect(s.toast) {
        s.toast?.let {
            vm.toastShown()
            snackbar.showSnackbar(it)
        }
    }

    if (fullscreen) BackHandler { rotation.exitFullscreen() }

    // full screen: the bars are left out; the full screen view has its own
    // SurfaceView, so the preview reconnects once on each switch (well under a
    // second: the stream starts at a key frame)
    Scaffold(
        topBar = {
            if (!fullscreen) TopAppBar(
                title = { Text(stringResource(R.string.app_name)) },
                actions = {
                    ConnBadge(s.conn)
                    IconButton(onClick = {
                        when (tab) {
                            3 -> vm.refreshDvr()
                            else -> if (s.connected) vm.reload() else vm.retry()
                        }
                    }) { Icon(Icons.Filled.Refresh, stringResource(R.string.refresh)) }
                },
            )
        },
        bottomBar = {
            if (!fullscreen) NavigationBar {
                NavigationBarItem(tab == 0, { tab = 0 }, { Icon(Icons.Filled.Info, null) },
                    label = { Text(stringResource(R.string.tab_status)) })
                NavigationBarItem(tab == 1, { tab = 1 }, { Icon(Icons.Filled.PlayArrow, null) },
                    label = { Text(stringResource(R.string.tab_preview)) })
                NavigationBarItem(tab == 2, { tab = 2 }, { Icon(Icons.Filled.Settings, null) },
                    label = { Text(stringResource(R.string.tab_settings)) })
                NavigationBarItem(tab == 3, { tab = 3; vm.refreshLocal() }, { Icon(Icons.AutoMirrored.Filled.List, null) },
                    label = { Text(stringResource(R.string.tab_dvr)) })
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { pad ->
        if (fullscreen) {
            Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
                VideoSurface(s, vm, Modifier, fullscreen = true, onExitFullscreen = rotation::exitFullscreen)
            }
            return@Scaffold
        }
        val m = Modifier.padding(pad)
        when (tab) {
            0 -> StatusScreen(s, vm, m)
            1 -> PreviewScreen(s, vm, m, onFullscreen = rotation::enterFullscreen)
            2 -> SettingsScreen(s, vm, m)
            else -> DvrScreen(s, vm, m)
        }
    }
}
