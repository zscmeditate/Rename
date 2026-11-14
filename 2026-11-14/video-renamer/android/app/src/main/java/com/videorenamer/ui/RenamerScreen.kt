package com.videorenamer.ui

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.videorenamer.RenameEngine
import com.videorenamer.ui.components.AppCard
import com.videorenamer.ui.theme.AppColors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val INITIAL_LOG =
    "请选择要处理的文件夹并授权访问。\r\n\r\n" +
        "规则：当前文件夹（不含子文件夹）中的 .mp4 / .mkv 文件，按文件名（不含扩展名）" +
        "以与资源管理器一致的顺序排序，依次重命名为 01、02、03……并保留各自原扩展名；" +
        "文件超过 99 个时统一改用三位编号 001、002、003……\r\n\r\n" +
        "重命名后可点击「撤销重命名」恢复原文件名。\r\n" +
        "Android 只能访问你在这里授权的文件夹。"

@Composable
fun RenamerScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var treeUri by remember { mutableStateOf<Uri?>(null) }
    var folderName by remember { mutableStateOf("") }
    var folderPath by remember { mutableStateOf("") }
    var log by remember { mutableStateOf(INITIAL_LOG) }
    var busy by remember { mutableStateOf(false) }
    var undoEntries by remember { mutableStateOf<List<RenameEngine.UndoEntry>>(emptyList()) }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) {
            try {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                )
            } catch (_: SecurityException) {
                // 个别提供方不支持持久化授权，本次仍可操作
            }
            treeUri = uri
            folderName = RenameEngine.folderName(uri)
            folderPath = RenameEngine.folderPath(uri)
            undoEntries = emptyList()
            log = "已选择文件夹「$folderName」。\r\n\r\n点击「重命名」开始处理。"
        }
    }

    val startRename: () -> Unit = {
        val uri = treeUri
        if (uri != null && !busy) {
            busy = true
            log = "正在重命名，请稍候……"
            scope.launch {
                val outcome = withContext(Dispatchers.IO) {
                    runCatching { RenameEngine.renameAll(context, uri) }
                }
                busy = false
                outcome.onSuccess { result ->
                    if (result.done.isNotEmpty()) undoEntries = result.done
                    log = buildRenameLog(result, folderPath)
                }.onFailure { e ->
                    log = "重命名失败：${e.message ?: e.javaClass.simpleName}"
                }
            }
        }
    }

    val startUndo: () -> Unit = {
        val uri = treeUri
        val entries = undoEntries
        if (uri != null && entries.isNotEmpty() && !busy) {
            busy = true
            log = "正在撤销，请稍候……"
            scope.launch {
                val outcome = withContext(Dispatchers.IO) {
                    runCatching { RenameEngine.undo(context, uri, entries) }
                }
                busy = false
                outcome.onSuccess { result ->
                    log = buildUndoLog(result, folderPath)
                    if (result.errors.isEmpty()) undoEntries = emptyList()
                }.onFailure { e ->
                    log = "撤销失败：${e.message ?: e.javaClass.simpleName}"
                }
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(AppColors.Background)
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(horizontal = 20.dp)
    ) {
        Spacer(Modifier.height(28.dp))

        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    text = "视频批量重命名",
                    style = MaterialTheme.typography.headlineMedium,
                    color = AppColors.TextPrimary
                )
            }
            if (busy) {
                CircularProgressIndicator(
                    modifier = Modifier.size(22.dp),
                    color = AppColors.Primary,
                    strokeWidth = 2.dp
                )
            }
        }

        Spacer(Modifier.height(32.dp))

        SectionLabel("当前文件夹")
        Spacer(Modifier.height(8.dp))
        AppCard {
            FolderRow(
                name = folderName,
                path = folderPath,
                selected = treeUri != null
            )
        }

        Spacer(Modifier.height(18.dp))

        NeonButton(
            text = if (busy) "处理中…" else "选择文件夹",
            modifier = Modifier.fillMaxWidth(),
            enabled = !busy,
            onClick = { picker.launch(null) }
        )

        Spacer(Modifier.height(12.dp))

        Row(Modifier.fillMaxWidth()) {
            NeonButton(
                text = "重命名",
                modifier = Modifier.weight(1f),
                enabled = treeUri != null && !busy,
                onClick = startRename
            )
            Spacer(Modifier.width(10.dp))
            GhostButton(
                text = "撤销重命名",
                modifier = Modifier.weight(1f),
                enabled = undoEntries.isNotEmpty() && !busy,
                onClick = startUndo
            )
        }

        Spacer(Modifier.height(26.dp))

        SectionLabel("处理日志")
        Spacer(Modifier.height(8.dp))

        AppCard(modifier = Modifier.weight(1f)) {
            val scroll = rememberScrollState()
            Column(
                Modifier
                    .fillMaxSize()
                    .verticalScroll(scroll)
                    .padding(18.dp)
            ) {
                SelectionContainer {
                    Text(
                        text = log,
                        style = MaterialTheme.typography.bodySmall.copy(lineHeight = 20.sp),
                        color = AppColors.TextPrimary
                    )
                }
            }
        }

        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelLarge,
        color = AppColors.TextSecondary,
        fontWeight = FontWeight.Bold
    )
}

@Composable
private fun FolderRow(name: String, path: String, selected: Boolean) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 17.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(40.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(AppColors.SurfaceSoft),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = Icons.Default.FolderOpen,
                contentDescription = null,
                tint = AppColors.Primary,
                modifier = Modifier.size(22.dp)
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = name.ifEmpty { "尚未选择文件夹" },
                style = MaterialTheme.typography.bodyLarge,
                color = if (selected) AppColors.TextPrimary else AppColors.TextSecondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            if (path.isNotEmpty()) {
                Spacer(Modifier.height(2.dp))
                Text(
                    text = path,
                    style = MaterialTheme.typography.bodySmall,
                    color = AppColors.TextSecondary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        Spacer(Modifier.width(10.dp))
        StatusPill(selected)
    }
}

@Composable
private fun StatusPill(selected: Boolean) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(10.dp))
            .background(AppColors.SurfaceSoft)
            .padding(horizontal = 10.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            Modifier
                .size(6.dp)
                .clip(CircleShape)
                .background(if (selected) AppColors.Primary else AppColors.Disabled)
        )
        Spacer(Modifier.width(5.dp))
        Text(
            text = if (selected) "已选择" else "未选择",
            style = MaterialTheme.typography.labelSmall,
            color = if (selected) AppColors.Primary else AppColors.TextSecondary
        )
    }
}

@Composable
private fun NeonButton(
    text: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    Box(
        modifier = modifier
            .height(52.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(if (enabled) AppColors.Primary else AppColors.SurfaceElevated)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.titleMedium,
            color = if (enabled) AppColors.Background else AppColors.Disabled
        )
    }
}

@Composable
private fun GhostButton(
    text: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    Box(
        modifier = modifier
            .height(52.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(AppColors.Surface)
            .border(
                width = 1.dp,
                color = if (enabled) AppColors.BorderStrong else AppColors.BorderSubtle,
                shape = RoundedCornerShape(14.dp)
            )
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.titleMedium,
            color = if (enabled) AppColors.TextPrimary else AppColors.Disabled
        )
    }
}

private fun buildRenameLog(result: RenameEngine.RenameResult, path: String): String {
    if (result.total == 0) {
        return "该文件夹中未找到 .mp4 或 .mkv 文件。\r\n\r\n目录：\r\n$path"
    }
    val sb = StringBuilder()
    sb.append("处理完成：成功重命名 ${result.done.size} 个文件")
    if (result.errors.isNotEmpty()) sb.append("，失败 ${result.errors.size} 个")
    sb.append("。\r\n")
    sb.append("共找到 ${result.total} 个视频文件，目录：\r\n$path\r\n")

    if (result.done.isNotEmpty()) {
        sb.append("\r\n重命名明细（原文件名 → 新文件名）：\r\n")
        result.done.forEach { sb.append("· ${it.original} → ${it.current}\r\n") }
        sb.append("\r\n如需恢复原文件名，可点击「撤销重命名」。\r\n")
    }
    if (result.errors.isNotEmpty()) {
        sb.append("\r\n失败详情：\r\n")
        result.errors.forEach { sb.append("· $it\r\n") }
    }
    return sb.toString().trimEnd()
}

private fun buildUndoLog(result: RenameEngine.UndoResult, path: String): String {
    val sb = StringBuilder()
    sb.append("撤销完成：已恢复 ${result.restored} 个文件的原名")
    if (result.errors.isNotEmpty()) sb.append("，失败 ${result.errors.size} 个")
    sb.append("。\r\n目录：\r\n$path\r\n")

    if (result.done.isNotEmpty()) {
        sb.append("\r\n恢复明细（编号名 → 原文件名）：\r\n")
        result.done.forEach { sb.append("· ${it.current} → ${it.original}\r\n") }
    }
    if (result.skipped.isNotEmpty()) {
        sb.append("\r\n另有 ${result.skipped.size} 个文件已不存在，已跳过。\r\n")
    }
    if (result.errors.isNotEmpty()) {
        sb.append("\r\n详情：\r\n")
        result.errors.forEach { sb.append("· $it\r\n") }
        sb.append("\r\n仍有文件保持编号名称，可再次尝试撤销。\r\n")
    }
    return sb.toString().trimEnd()
}
