package io.github.kkbin505.rkfpv.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import io.github.kkbin505.rkfpv.R

@Composable
fun ConnBadge(c: ConnState) {
    val (text, color) = when (c) {
        is ConnState.Connected -> stringResource(R.string.conn_connected) to Color(0xFF2E7D32)
        ConnState.Connecting, ConnState.AwaitingAuth -> stringResource(R.string.conn_connecting) to Color(0xFFF9A825)
        else -> stringResource(R.string.conn_none) to MaterialTheme.colorScheme.outline
    }
    Text(text, color = color, style = MaterialTheme.typography.labelLarge, modifier = Modifier.padding(end = 4.dp))
}

/** Shown on every tab while there is no board connection. */
@Composable
fun ConnectCard(s: UiState, vm: BoardViewModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            when (val c = s.conn) {
                ConnState.NoBoard -> {
                    Text(stringResource(R.string.no_board_title), style = MaterialTheme.typography.titleMedium)
                    Text(stringResource(R.string.no_board_help))
                    OutlinedButton(onClick = vm::retry) { Text(stringResource(R.string.retry)) }
                }
                is ConnState.NeedPermission -> {
                    Text(stringResource(R.string.need_permission))
                    Button(onClick = { vm.requestPermission(c.device) }) { Text(stringResource(R.string.allow_usb)) }
                }
                ConnState.Connecting -> Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(12.dp))
                    Text(stringResource(R.string.connecting))
                }
                ConnState.AwaitingAuth -> Text(stringResource(R.string.awaiting_auth))
                is ConnState.Failed -> {
                    Text(stringResource(R.string.conn_failed), style = MaterialTheme.typography.titleMedium)
                    Text(c.message, color = MaterialTheme.colorScheme.error)
                    if (c.missingFpv) Text(stringResource(R.string.missing_fpv_help))
                    OutlinedButton(onClick = vm::retry) { Text(stringResource(R.string.retry)) }
                }
                is ConnState.Connected -> {}
            }
        }
    }
}

@Composable
fun StatusScreen(s: UiState, vm: BoardViewModel, modifier: Modifier) {
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!s.connected) {
            ConnectCard(s, vm)
            return@Column
        }
        val st = s.status
        if (st == null) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
            return@Column
        }
        if (!st.fpvRunning) {
            Card(Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.debug_mode_note), Modifier.padding(16.dp))
            }
        }
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Field(stringResource(R.string.st_mode), "${st.mode}  (boot: ${st["override"]}, usb: ${st["usb_role"].ifEmpty { "?" }})")
                Field(stringResource(R.string.st_video), if (st["fps"].isNotEmpty()) "${st["fps"]} fps, ${st["kbps"]} kbps" else "-")
                Field(stringResource(R.string.st_adapter), st["driver"].ifEmpty { stringResource(R.string.none) } +
                    if (st.txpowerKey.isNotEmpty()) "  ${st.txpowerKey} = ${st["txpower"]}" else "")
                Field(stringResource(R.string.st_radio), st["radio"].ifEmpty { "-" })
                Field(stringResource(R.string.st_memory), st["mem_avail_kb"].ifEmpty { "?" } + " kB")
                Field(stringResource(R.string.st_temp), st["temp_c"].ifEmpty { "?" } + " °C")
                Field(stringResource(R.string.st_uptime), st["uptime_s"].ifEmpty { "?" } + " s")
                Field(stringResource(R.string.st_dvr), st["dvr_state"] +
                    st["dvr_file"].let { if (it.isNotEmpty() && it != "-") "  $it" else "" })
            }
        }
        val c = s.conn
        if (c is ConnState.Connected && c.banner.isNotEmpty()) {
            Text(c.banner, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.outline)
        }
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun Field(label: String, value: String) {
    Row {
        Text(label, Modifier.width(96.dp), color = MaterialTheme.colorScheme.outline)
        Text(value, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium)
    }
}
