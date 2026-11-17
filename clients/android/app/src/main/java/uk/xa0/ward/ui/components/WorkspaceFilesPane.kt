package uk.xa0.ward.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.ArrowUpward
import androidx.compose.material.icons.rounded.Description
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material.icons.rounded.Image
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import uk.xa0.ward.diag.DiagLazyList
import uk.xa0.ward.diag.diagDrag
import uk.xa0.ward.diag.diagOffset
import uk.xa0.ward.model.PreviewFormat
import uk.xa0.ward.model.WorkspaceFilesState
import uk.xa0.ward.model.WorkspaceRow
import uk.xa0.ward.model.previewFormatOf
import uk.xa0.ward.ui.clickableNoRipple
import uk.xa0.ward.ui.shortenPath
import uk.xa0.ward.ui.theme.DshRadius
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class WorkspaceFilesWiring(
    val state: WorkspaceFilesState,
    val onRefresh: () -> Unit,
    val onEnter: (String) -> Unit,
    val onUp: () -> Unit,
)

@Composable
internal fun WorkspaceFilesPane(
    root: String,
    wiring: WorkspaceFilesWiring,
    selectedPath: String?,
    onSelectFile: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = DshTheme.colors
    val state = wiring.state
    val currentPath = when (state) {
        is WorkspaceFilesState.Ready -> state.currentPath
        is WorkspaceFilesState.Loading -> state.path
        is WorkspaceFilesState.Failed -> state.path
        WorkspaceFilesState.Idle -> ""
    }
    val rows = (state as? WorkspaceFilesState.Ready)?.rows.orEmpty()
    val timeFormat = remember { SimpleDateFormat("d MMM HH:mm", Locale.getDefault()) }

    LaunchedEffect(state) {
        if (state is WorkspaceFilesState.Idle) wiring.onRefresh()
    }

    Column(modifier) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = DshSpacing.lg, vertical = DshSpacing.xs),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(DshSpacing.sm),
        ) {
            if (currentPath.isNotEmpty()) {
                Icon(
                    Icons.Rounded.ArrowUpward,
                    contentDescription = "Up to workspace root",
                    tint = colors.labelSecondary,
                    modifier = Modifier
                        .size(28.dp)
                        .clip(RoundedCornerShape(DshRadius.sm))
                        .clickableNoRipple(onClick = wiring.onUp)
                        .padding(DshSpacing.xs),
                )
            }
            Text(
                text = if (currentPath.isEmpty()) shortenPath(root) else currentPath,
                style = DshType.bodySmall,
                color = colors.labelSecondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = "Refresh",
                style = DshType.bodySmall,
                color = colors.link,
                maxLines = 1,
                modifier = Modifier
                    .clip(RoundedCornerShape(DshRadius.sm))
                    .clickableNoRipple(onClick = wiring.onRefresh)
                    .padding(horizontal = DshSpacing.sm, vertical = DshSpacing.xs),
            )
        }
        Box(Modifier.fillMaxWidth().height(0.5.dp).background(colors.borderL1))
        Box(Modifier.fillMaxWidth().weight(1f)) {
            when {
                state is WorkspaceFilesState.Failed -> WorkspaceFilesNote(
                    message = state.message,
                    action = "Retry",
                    onAction = wiring.onRefresh,
                    modifier = Modifier.fillMaxSize(),
                )

                state is WorkspaceFilesState.Loading || state is WorkspaceFilesState.Idle ->
                    WorkspaceFilesNote("Loading…", modifier = Modifier.fillMaxSize())

                rows.isEmpty() ->
                    WorkspaceFilesNote("Empty directory", modifier = Modifier.fillMaxSize())

                else -> {
                    val listState = rememberLazyListState()
                    DiagLazyList("files:workspace", listState)
                    LazyColumn(
                        Modifier
                            .fillMaxSize()
                            .diagDrag("files:workspace") { listState.diagOffset() },
                        state = listState,
                    ) {
                        items(rows, key = { it.path }) { row ->
                            WorkspaceFileRow(
                                row = row,
                                selected = row.path == selectedPath,
                                time = row.mtimeMillis?.let { timeFormat.format(Date(it)) },
                                onEnter = { wiring.onEnter(row.path) },
                                onOpen = { onSelectFile(row.path) },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun WorkspaceFileRow(
    row: WorkspaceRow,
    selected: Boolean,
    time: String?,
    onEnter: () -> Unit,
    onOpen: () -> Unit,
) {
    val colors = DshTheme.colors
    val icon = when {
        row.directory -> Icons.Rounded.Folder
        previewFormatOf(row.name) == PreviewFormat.TEXT -> Icons.Rounded.Description
        else -> Icons.Rounded.Image
    }
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(DshRadius.md))
            .background(if (selected) colors.sidebarItemActive else colors.bgBase)
            .clickableNoRipple(enabled = row.openable) {
                if (row.directory) onEnter() else onOpen()
            }
            .padding(
                start = DshSpacing.lg + (row.depth * 12).dp,
                end = DshSpacing.lg,
                top = DshSpacing.sm,
                bottom = DshSpacing.sm,
            ),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(DshSpacing.md),
    ) {
        Icon(
            imageVector = icon,
            contentDescription = if (row.directory) "Directory" else "File",
            tint = if (row.openable) colors.labelTertiary else colors.labelCaption,
            modifier = Modifier.size(16.dp),
        )
        Text(
            text = row.name,
            style = DshType.labelMedium,
            color = if (row.openable && !row.hidden) colors.labelPrimary else colors.labelTertiary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        row.size?.let { size ->
            Text(
                text = formatWorkspaceSize(size),
                style = DshType.bodySmall,
                color = colors.labelTertiary,
                maxLines = 1,
            )
        }
        time?.let {
            Text(
                text = it,
                style = DshType.micro,
                color = colors.labelCaption,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun WorkspaceFilesNote(
    message: String,
    modifier: Modifier = Modifier,
    action: String? = null,
    onAction: (() -> Unit)? = null,
) {
    val colors = DshTheme.colors
    Column(
        modifier.padding(DshSpacing.xxl),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(DshSpacing.sm),
    ) {
        Text(
            text = message,
            style = DshType.bodySmall,
            color = colors.labelTertiary,
            textAlign = TextAlign.Center,
        )
        if (action != null && onAction != null) {
            Text(
                text = action,
                style = DshType.bodySmall,
                color = colors.link,
                modifier = Modifier
                    .clip(RoundedCornerShape(DshRadius.sm))
                    .clickableNoRipple(onClick = onAction)
                    .padding(DshSpacing.xs),
            )
        }
    }
}

private fun formatWorkspaceSize(bytes: Long): String = when {
    bytes >= 1024L * 1024 * 1024 -> "%.1f GB".format(bytes / (1024.0 * 1024 * 1024))
    bytes >= 1024L * 1024 -> "%.1f MB".format(bytes / (1024.0 * 1024))
    bytes >= 1024L -> "%.0f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}
