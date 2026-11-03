package uk.xa0.ward.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import uk.xa0.ward.HtopTerminal
import uk.xa0.ward.term.HtopPhase
import uk.xa0.ward.term.HtopState
import uk.xa0.ward.term.TerminalCellSize
import uk.xa0.ward.term.htopPhaseWord
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

@Composable
internal fun HtopPanel(controller: HtopTerminal, sessionId: String, modifier: Modifier = Modifier) {
    val state by controller.state.collectAsState()
    val fontSp by controller.font.collectAsState()
    val emulator = controller.screen()
    val cellSize = TerminalCellSize.of(fontSp)

    LaunchedEffect(sessionId) { controller.start(sessionId) }

    Column(modifier.fillMaxSize().background(DshTheme.colors.bgBase)) {
        HtopBar(state = state, onRetry = controller::retry)
        Box(Modifier.fillMaxWidth().weight(1f)) {
            if (emulator != null && surfacePhase(state.phase)) {
                TerminalSurface(
                    emulator = emulator,
                    revision = state.revision,
                    cellSize = cellSize,
                    onCellSize = controller::setFont,
                    onWrite = controller::write,
                    onResize = controller::resize,
                )
            } else {
                HtopPlaceholder(state = state, onRetry = controller::retry)
            }
        }
    }
}

private fun surfacePhase(phase: HtopPhase): Boolean =
    phase == HtopPhase.RUNNING || phase == HtopPhase.DISCONNECTED

@Composable
private fun HtopBar(state: HtopState, onRetry: () -> Unit) {
    val colors = DshTheme.colors
    Column {
        Row(
            Modifier.fillMaxWidth().height(34.dp).padding(horizontal = DshSpacing.md),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Htop",
                style = DshType.bodySmall.copy(fontWeight = FontWeight.Medium),
                color = colors.labelPrimary,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = htopPhaseWord(state.phase),
                style = DshType.micro,
                color = if (state.phase == HtopPhase.RUNNING) colors.success else colors.labelTertiary,
            )
            if (state.phase == HtopPhase.FAILED || state.phase == HtopPhase.DISCONNECTED) {
                Spacer(Modifier.width(DshSpacing.md))
                BarAction("Reconnect", onRetry)
            }
        }
        state.error?.let { message ->
            Text(
                text = message,
                style = DshType.micro,
                color = colors.labelSecondary,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(colors.bgLayer2)
                    .padding(horizontal = DshSpacing.md, vertical = DshSpacing.sm),
            )
        }
    }
}

@Composable
private fun HtopPlaceholder(state: HtopState, onRetry: () -> Unit) {
    val colors = DshTheme.colors
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.padding(DshSpacing.xl),
        ) {
            Text(
                text = when (state.phase) {
                    HtopPhase.STARTING -> "Opening the terminal…"
                    HtopPhase.CONNECTING -> "Attaching to Htop…"
                    HtopPhase.DISCONNECTED -> "The Htop stream stopped."
                    HtopPhase.FAILED -> "No Htop terminal."
                    else -> "Opening Htop…"
                },
                style = DshType.bodySmall,
                color = colors.labelSecondary,
            )
            if (state.phase == HtopPhase.FAILED || state.phase == HtopPhase.DISCONNECTED) {
                Spacer(Modifier.height(DshSpacing.md))
                BarAction("Reconnect", onRetry)
            }
        }
    }
}
