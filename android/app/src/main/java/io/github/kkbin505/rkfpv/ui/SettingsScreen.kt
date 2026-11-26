package io.github.kkbin505.rkfpv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.ScrollableTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import io.github.kkbin505.rkfpv.R
import io.github.kkbin505.rkfpv.fpv.FpvParam
import io.github.kkbin505.rkfpv.fpv.SetResult

@Composable
fun SettingsScreen(s: UiState, vm: BoardViewModel, modifier: Modifier) {
    if (!s.connected || s.schema.isEmpty()) {
        Column(modifier.padding(16.dp)) { ConnectCard(s, vm) }
        return
    }
    val groups = s.groups
    var groupName by rememberSaveable { mutableStateOf(groups.first()) }
    val gi = groups.indexOf(groupName).coerceAtLeast(0)
    var confirmLink by remember { mutableStateOf(false) }
    var showPreview by rememberSaveable { mutableStateOf(false) }

    Column(modifier.fillMaxSize()) {
        // watch the picture while tuning (Picture / Exposure are live)
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.show_preview), Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Switch(checked = showPreview, onCheckedChange = {
                showPreview = it
                if (it && !s.videoRunning) vm.setVideo(true)
            })
        }
        if (showPreview && s.videoRunning) VideoSurface(s, vm, Modifier.fillMaxWidth())
        ScrollableTabRow(selectedTabIndex = gi, edgePadding = 8.dp) {
            groups.forEachIndexed { i, g ->
                val dirty = s.edits.keys.any { s.param(it)?.group == g }
                Tab(i == gi, onClick = { groupName = g }, text = { Text(if (dirty) "$g •" else g) })
            }
        }
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            val params = s.schema.filter { it.group == groups[gi] }
            items(params, key = { it.key }) { p ->
                ParamRow(p, s, vm)
                HorizontalDivider()
            }
            if (groups[gi] == "Radio" || groups[gi] == "Telemetry") {
                item { Text(stringResource(R.string.link_note), Modifier.padding(16.dp), style = MaterialTheme.typography.bodySmall) }
            }
        }
        if (s.edits.isNotEmpty()) {
            val invalid = s.invalidEdits
            Surface(tonalElevation = 3.dp) {
                Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        if (invalid.isNotEmpty()) stringResource(R.string.fix_fields, invalid.joinToString(", "))
                        else stringResource(R.string.unsaved, s.edits.size),
                        Modifier.weight(1f),
                        color = if (invalid.isNotEmpty()) MaterialTheme.colorScheme.error else Color.Unspecified,
                    )
                    TextButton(onClick = vm::discardEdits) { Text(stringResource(R.string.discard)) }
                    Button(
                        enabled = invalid.isEmpty() && !s.saving,
                        onClick = {
                            if (s.edits.keys.any { s.param(it)?.isLink == true }) confirmLink = true else vm.save()
                        },
                    ) { Text(stringResource(R.string.save)) }
                }
            }
        }
    }

    if (confirmLink) {
        AlertDialog(
            onDismissRequest = { confirmLink = false },
            title = { Text(stringResource(R.string.link_confirm_title)) },
            text = { Text(stringResource(R.string.link_confirm_text, s.edits.keys.filter { s.param(it)?.isLink == true }.joinToString(", "))) },
            confirmButton = { TextButton(onClick = { confirmLink = false; vm.save() }) { Text(stringResource(R.string.save)) } },
            dismissButton = { TextButton(onClick = { confirmLink = false }) { Text(stringResource(R.string.cancel)) } },
        )
    }
    s.setResult?.let { SetResultDialog(it, vm) }
}

@Composable
private fun ApplyTag(p: FpvParam) {
    val (text, color) = when {
        p.isLive -> stringResource(R.string.tag_live) to Color(0xFF2E7D32)
        p.isLink -> stringResource(R.string.tag_link) to Color(0xFFC62828)
        p.apply == "boot" -> stringResource(R.string.tag_boot) to Color(0xFF6A1B9A)
        else -> stringResource(R.string.tag_restart) to Color(0xFFEF6C00)
    }
    Text(
        text, color = Color.White, style = MaterialTheme.typography.labelSmall,
        modifier = Modifier.background(color, RoundedCornerShape(4.dp)).padding(horizontal = 6.dp, vertical = 1.dp),
    )
}

@Composable
private fun ParamRow(p: FpvParam, s: UiState, vm: BoardViewModel) {
    val saved = s.values[p.key] ?: p.value
    val value = s.edits[p.key] ?: saved
    val changed = p.key in s.edits
    val bg = if (changed) MaterialTheme.colorScheme.primary.copy(alpha = 0.10f) else Color.Transparent
    Column(Modifier.fillMaxWidth().background(bg).padding(horizontal = 16.dp, vertical = 10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(p.key, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f, fill = false))
            Spacer(Modifier.width(8.dp))
            ApplyTag(p)
            if (p.key == s.status?.txpowerKey) {
                Spacer(Modifier.width(4.dp))
                Text(stringResource(R.string.tag_in_use), style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary)
            }
        }
        Box(Modifier.padding(vertical = 6.dp)) { ParamControl(p, value) { vm.edit(p.key, it) } }
        val range = if (p.type == "int") " · ${p.min}–${p.max}" else ""
        Text("${p.help} (default ${p.default}$range)", style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.outline)
    }
}

@Composable
private fun ParamControl(p: FpvParam, value: String, onChange: (String) -> Unit) {
    when {
        p.type == "bool" && p.specials.isEmpty() ->
            Switch(checked = value == "1", onCheckedChange = { onChange(if (it) "1" else "0") })
        p.type == "enum" -> Choice(value, p.choices, onChange)
        else -> Row(verticalAlignment = Alignment.CenterVertically) {
            val special = value in p.specials
            if (p.specials.isNotEmpty()) {
                val number = stringResource(R.string.number)
                Choice(if (special) value else number, p.specials + number) { w ->
                    if (w == number) onChange(if (special) p.min.toString() else value) else onChange(w)
                }
                Spacer(Modifier.width(8.dp))
            }
            if (!special) {
                val err = p.validate(value)
                OutlinedTextField(
                    value = value, onValueChange = { onChange(it.trim()) }, singleLine = true,
                    isError = err != null,
                    supportingText = err?.let { { Text(it) } },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.width(160.dp),
                )
            }
        }
    }
}

@Composable
private fun Choice(value: String, options: List<String>, onChange: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { open = true }) { Text("$value  ▾") }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            options.forEach { o ->
                DropdownMenuItem(text = { Text(o) }, onClick = { open = false; onChange(o) })
            }
        }
    }
}

@Composable
private fun SetResultDialog(r: SetResult, vm: BoardViewModel) {
    val restart = r.restartNeeded()
    AlertDialog(
        onDismissRequest = vm::dismissSetResult,
        title = { Text(stringResource(R.string.saved_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                r.results.forEach { Text("${it.key} = ${it.value}  → ${it.result}") }
                if (r.warning.isNotEmpty()) Text("⚠ ${r.warning}", color = MaterialTheme.colorScheme.error)
                if (!r.fpvRunning) Text(stringResource(R.string.saved_debug_mode))
            }
        },
        confirmButton = {
            if (restart != null) TextButton(onClick = { vm.restart(restart) }) { Text(stringResource(R.string.restart_what, restart)) }
            else TextButton(onClick = vm::dismissSetResult) { Text(stringResource(R.string.ok)) }
        },
        dismissButton = if (restart != null) {
            { TextButton(onClick = vm::dismissSetResult) { Text(stringResource(R.string.later)) } }
        } else null,
    )
}
