package io.github.kkbin505.rkfpv.ui

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import io.github.kkbin505.rkfpv.R
import io.github.kkbin505.rkfpv.fpv.DvrFile
import java.io.File

private fun mb(bytes: Long) = "%.1f MB".format(bytes / 1048576.0)

private fun mime(name: String) = if (name.endsWith(".ts")) "video/mp2t" else "video/mp4"

private fun uri(ctx: Context, f: File) = FileProvider.getUriForFile(ctx, "${ctx.packageName}.files", f)

private fun play(ctx: Context, f: File, vm: BoardViewModel) {
    val i = Intent(Intent.ACTION_VIEW).setDataAndType(uri(ctx, f), mime(f.name))
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    try {
        ctx.startActivity(i)
    } catch (e: ActivityNotFoundException) {
        vm.toast(ctx.getString(R.string.no_player))
    }
}

private fun share(ctx: Context, f: File) {
    val i = Intent(Intent.ACTION_SEND).setType(mime(f.name))
        .putExtra(Intent.EXTRA_STREAM, uri(ctx, f))
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    ctx.startActivity(Intent.createChooser(i, f.name))
}

private sealed interface PendingDelete {
    data class Board(val name: String) : PendingDelete
    data class Local(val rec: LocalRecording) : PendingDelete
}

@Composable
fun DvrScreen(s: UiState, vm: BoardViewModel, modifier: Modifier) {
    val ctx = LocalContext.current
    var pending by remember { mutableStateOf<PendingDelete?>(null) }
    LaunchedEffect(s.connected) { if (s.connected) vm.refreshDvr() }
    val localByName = s.local.associateBy { it.name }

    LazyColumn(
        modifier.fillMaxSize().padding(horizontal = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (s.connected) item {
            Column(Modifier.padding(top = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                VideoSwitch(s, vm)
                RecordControl(s, vm)
            }
        }
        item {
            Text(stringResource(R.string.dvr_board), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 16.dp))
        }
        val dvr = s.dvr
        when {
            !s.connected -> item { ConnectCard(s, vm) }
            dvr == null -> item { Text(if (s.dvrLoading) "…" else stringResource(R.string.dvr_no_card)) }
            else -> {
                item {
                    Text(stringResource(R.string.dvr_space, dvr.freeMb, dvr.totalMb),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.outline)
                }
                if (dvr.files.isEmpty()) item { Text(stringResource(R.string.dvr_empty)) }
                items(dvr.files, key = { "b:" + it.name }) { f ->
                    BoardFileRow(f, s.downloads[f.name], localByName[f.name], vm, ctx) { pending = PendingDelete.Board(f.name) }
                }
            }
        }
        item {
            Text(stringResource(R.string.dvr_phone), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 16.dp))
        }
        if (s.local.isEmpty()) item { Text(stringResource(R.string.dvr_phone_empty)) }
        items(s.local, key = { "l:" + it.name }) { r ->
            Card(Modifier.fillMaxWidth()) {
                Row(Modifier.padding(start = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(r.name, fontWeight = FontWeight.SemiBold)
                        Text(mb(r.size), style = MaterialTheme.typography.bodySmall)
                    }
                    IconButton(onClick = { play(ctx, r.file, vm) }) { Icon(Icons.Filled.PlayArrow, stringResource(R.string.play)) }
                    IconButton(onClick = { share(ctx, r.file) }) { Icon(Icons.Filled.Share, stringResource(R.string.share)) }
                    IconButton(onClick = { pending = PendingDelete.Local(r) }) { Icon(Icons.Filled.Delete, stringResource(R.string.delete)) }
                }
            }
        }
        item {
            Text(stringResource(R.string.dvr_phone_dir, vm.dvrDir.path), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.outline, modifier = Modifier.padding(vertical = 16.dp))
        }
    }

    pending?.let { p ->
        val name = when (p) { is PendingDelete.Board -> p.name; is PendingDelete.Local -> p.rec.name }
        AlertDialog(
            onDismissRequest = { pending = null },
            title = { Text(stringResource(if (p is PendingDelete.Board) R.string.delete_board_title else R.string.delete_phone_title)) },
            text = { Text(name) },
            confirmButton = {
                TextButton(onClick = {
                    when (p) {
                        is PendingDelete.Board -> vm.deleteOnBoard(p.name)
                        is PendingDelete.Local -> vm.deleteLocal(p.rec)
                    }
                    pending = null
                }) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = { TextButton(onClick = { pending = null }) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

@Composable
private fun BoardFileRow(
    f: DvrFile, progress: Long?, local: LocalRecording?, vm: BoardViewModel, ctx: Context, onDelete: () -> Unit,
) {
    val have = local != null && local.size == f.size
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(f.name, fontWeight = FontWeight.SemiBold)
                    Text(
                        mb(f.size) + when {
                            f.recording -> "  · " + stringResource(R.string.dvr_recording)
                            have -> "  · " + stringResource(R.string.dvr_on_phone)
                            else -> ""
                        },
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                when {
                    progress != null -> OutlinedButton(onClick = { vm.cancelDownload(f.name) }) { Text(stringResource(R.string.cancel)) }
                    have -> IconButton(onClick = { play(ctx, local!!.file, vm) }) { Icon(Icons.Filled.PlayArrow, stringResource(R.string.play)) }
                    !f.recording -> OutlinedButton(onClick = { vm.download(f.name) }) { Text(stringResource(R.string.download)) }
                }
                if (!f.recording && progress == null) {
                    IconButton(onClick = onDelete) { Icon(Icons.Filled.Delete, stringResource(R.string.delete)) }
                }
            }
            if (progress != null && f.size > 0) {
                LinearProgressIndicator(
                    progress = { (progress.toFloat() / f.size).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                )
                Text("${mb(progress)} / ${mb(f.size)}", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
