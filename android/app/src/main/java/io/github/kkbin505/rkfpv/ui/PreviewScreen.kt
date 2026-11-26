package io.github.kkbin505.rkfpv.ui

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import io.github.kkbin505.rkfpv.R
import io.github.kkbin505.rkfpv.video.PreviewState

@Composable
fun PreviewScreen(s: UiState, vm: BoardViewModel, modifier: Modifier) {
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!s.connected) {
            ConnectCard(s, vm)
            return@Column
        }
        VideoSwitch(s, vm)
        if (s.videoRunning) {
            VideoSurface(s, vm, Modifier.fillMaxWidth())
            PreviewStats(s)
            RecordControl(s, vm)
        }
        Text(stringResource(R.string.preview_note), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.outline)
    }
}

/** Video on/off: `fpv preview on|off` (debug mode only; in FPV mode the video always runs). */
@Composable
fun VideoSwitch(s: UiState, vm: BoardViewModel) {
    val fpvMode = s.status?.fpvRunning == true
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.video), fontWeight = FontWeight.SemiBold)
                Text(
                    stringResource(
                        when {
                            fpvMode -> R.string.video_fpv_mode
                            s.videoRunning -> R.string.video_running
                            else -> R.string.video_stopped
                        },
                    ),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            if (s.videoBusy) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(12.dp))
            }
            Switch(
                checked = s.videoRunning,
                enabled = !s.videoBusy && !fpvMode,
                onCheckedChange = { vm.setVideo(it) },
            )
        }
    }
}

/** The decoded link stream. The SurfaceView exists only while this is on screen. */
@Composable
fun VideoSurface(s: UiState, vm: BoardViewModel, modifier: Modifier) {
    val ps = s.preview
    val ratio = (ps as? PreviewState.Playing)?.takeIf { it.width > 0 && it.height > 0 }
        ?.let { it.width.toFloat() / it.height } ?: (16f / 9f)
    Box(modifier.aspectRatio(ratio).background(Color.Black), contentAlignment = Alignment.Center) {
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    keepScreenOn = true
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(h: SurfaceHolder) = vm.startPreview(h.surface)
                        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
                        // the decoder must let go of the surface before this returns
                        override fun surfaceDestroyed(h: SurfaceHolder) = vm.stopPreview(wait = true)
                    })
                }
            },
            modifier = Modifier.fillMaxSize(),
        )
        when (ps) {
            null, PreviewState.Waiting -> Text(stringResource(R.string.preview_waiting), color = Color.White)
            is PreviewState.Failed -> Text(ps.message, color = Color(0xFFFF8A80), modifier = Modifier.padding(16.dp))
            is PreviewState.Playing -> if (s.dvrStatus?.recording == true) {
                Row(
                    Modifier.align(Alignment.TopEnd).padding(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.size(10.dp).background(Color.Red, CircleShape))
                    Spacer(Modifier.width(4.dp))
                    Text("REC", color = Color.White, style = MaterialTheme.typography.labelMedium)
                }
            }
        }
    }
}

@Composable
private fun PreviewStats(s: UiState) {
    val p = s.preview as? PreviewState.Playing ?: return
    val board = s.status?.let { st -> if (st["kbps"].isNotEmpty()) "  ·  ${st["kbps"]} kbps" else "" } ?: ""
    Text(
        "${p.width}×${p.height}  ·  %.0f fps".format(p.fps) + board +
            if (p.dropped > 0) "  ·  " + stringResource(R.string.preview_dropped, p.dropped) else "",
        style = MaterialTheme.typography.bodySmall,
    )
}

/** Start / stop recording on the board's TF card (needs the video running and `dvr` = 1). */
@Composable
fun RecordControl(s: UiState, vm: BoardViewModel) {
    val d = s.dvrStatus
    val recording = d?.recording == true
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(R.string.recording), fontWeight = FontWeight.SemiBold)
                    Text(
                        when {
                            d == null -> "-"
                            recording -> "${d.file}  ·  %.1f MB".format(d.bytes / 1048576.0) +
                                if (d.drops > 0) "  ·  " + stringResource(R.string.dvr_drops, d.drops) else ""
                            else -> d.state + if (d.freeMb > 0) "  ·  " + stringResource(R.string.dvr_free, d.freeMb) else ""
                        },
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                val enabled = s.videoRunning && s.dvrEnabled && !s.videoBusy && d != null && d.state != "no-card"
                if (recording) {
                    Button(
                        onClick = { vm.setRecording(false) }, enabled = !s.videoBusy,
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFC62828)),
                    ) { Text(stringResource(R.string.rec_stop)) }
                } else {
                    OutlinedButton(onClick = { vm.setRecording(true) }, enabled = enabled) {
                        Box(Modifier.size(10.dp).background(if (enabled) Color.Red else Color.Gray, CircleShape))
                        Spacer(Modifier.width(6.dp))
                        Text(stringResource(R.string.rec_start))
                    }
                }
            }
            val hint = when {
                d?.error?.isNotEmpty() == true -> d.error
                !s.dvrEnabled -> stringResource(R.string.rec_hint_dvr_off)
                !s.videoRunning -> stringResource(R.string.rec_hint_video_off)
                d?.state == "no-card" -> stringResource(R.string.dvr_no_card)
                else -> null
            }
            if (hint != null) Text(hint, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.outline)
        }
    }
}
