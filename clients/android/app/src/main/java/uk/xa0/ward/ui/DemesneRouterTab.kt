package uk.xa0.ward.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import uk.xa0.ward.model.EstateHealth
import uk.xa0.ward.model.EstateLoad
import uk.xa0.ward.model.EstateSlot
import uk.xa0.ward.model.EstateStatus
import uk.xa0.ward.model.RouterTarget
import uk.xa0.ward.ui.theme.DshRadius
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun DemesneRouterTab(
    target: RouterTarget?,
    status: EstateStatus?,
    error: String?,
    updatedAtMillis: Long?,
    modifier: Modifier = Modifier,
) {
    val colors = DshTheme.colors
    Column(
        modifier
            .fillMaxWidth()
            .background(colors.bgBase)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = DshSpacing.xl)
            .padding(top = DshSpacing.xl, bottom = DshSpacing.xxl),
        verticalArrangement = Arrangement.spacedBy(DshSpacing.sm),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("demesne", style = DshType.titleMedium, color = colors.labelPrimary)
            Spacer(Modifier.weight(1f))
            if (updatedAtMillis != null) {
                Text(
                    text = "updated ${clock(updatedAtMillis)}",
                    style = DshType.micro,
                    color = colors.labelCaption,
                )
            }
        }

        val subline = listOfNotNull(status?.version, status?.listen).joinToString(" · ")
        if (subline.isNotEmpty()) {
            Text(text = subline, style = DshType.bodySmall, color = colors.labelSecondary)
        }

        if (status == null) {
            Text(
                text = if (error != null) {
                    "The estate did not answer: $error"
                } else {
                    "Reading the estate…"
                },
                style = DshType.bodySmall,
                color = if (error != null) colors.error else colors.labelTertiary,
            )
        } else {
            LoadsSection(status.loads)
            EnginesSection(status.slots)
            EstateSection(status)
        }

        if (target != null) {
            SectionLabel("THIS SESSION")
            EstateRow("Model", target.modelId)
            EstateRow("Provider", "${target.provider.id} (${target.provider.displayName})")
            EstateRow("Endpoint", target.endpoint)
            EstateRow("Context window", target.contextWindow?.let(::tokenCount) ?: "Not declared")
            EstateRow("Max output", target.maxTokens?.let(::tokenCount) ?: "Not declared")
        }
    }
}

@Composable
private fun LoadsSection(loads: List<EstateLoad>) {
    val colors = DshTheme.colors
    SectionLabel("LOADS")
    if (loads.isEmpty()) {
        Text(
            text = "No model load in progress.",
            style = DshType.bodySmall,
            color = colors.labelTertiary,
        )
        return
    }
    for (load in loads) LoadCard(load)
}

@Composable
private fun LoadCard(load: EstateLoad) {
    val colors = DshTheme.colors
    val progress = load.progress?.takeIf { load.progressSource != null }
    EstateCard {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = load.model ?: load.slot,
                style = DshType.bodyMedium,
                color = colors.labelPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            if (progress != null) {
                Text(
                    text = "${(progress * 100).roundToInt()}%",
                    style = DshType.bodySmall,
                    color = colors.accent,
                )
            }
        }
        Text(
            text = listOfNotNull(load.slot, load.phase).joinToString(" · "),
            style = DshType.bodySmall,
            color = colors.labelSecondary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (progress != null) {
            LinearProgressIndicator(
                progress = { progress.coerceIn(0f, 1f) },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(3.dp)
                    .clip(RoundedCornerShape(DshRadius.pill)),
                color = colors.accent,
                trackColor = colors.borderL3,
            )
        }
        load.error?.let {
            Text(text = it, style = DshType.bodySmall, color = colors.error)
        }
        load.line?.let {
            Text(
                text = it,
                style = DshType.codeSmall,
                color = colors.labelTertiary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun EnginesSection(slots: List<EstateSlot>) {
    val colors = DshTheme.colors
    SectionLabel("ENGINES")
    val serving = slots.filter { slot -> !idleWord(slot.state) }
    if (serving.isEmpty()) {
        Text(
            text = "No engine is reported.",
            style = DshType.bodySmall,
            color = colors.labelTertiary,
        )
        return
    }
    for (slot in serving) EngineCard(slot)
}

@Composable
private fun EngineCard(slot: EstateSlot) {
    val colors = DshTheme.colors
    EstateCard {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = slot.name,
                style = DshType.bodyMedium,
                color = colors.labelPrimary,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = stateWord(slot),
                style = DshType.bodySmall,
                color = stateColor(slot),
            )
        }
        val placement = listOfNotNull(slot.device, slot.kind, slot.policy, slot.group).joinToString(" · ")
        if (placement.isNotEmpty()) EstateRow("Device", placement)
        slot.alias?.takeIf { it != slot.name }?.let { EstateRow("Alias", it) }
        slot.quant?.let { EstateRow("Quant", it) }
        slot.nCtx?.let { EstateRow("Context", tokenCount(it)) }
        slot.nParams?.let { EstateRow("Parameters", String.format(Locale.US, "%.1fB", it / 1_000_000_000.0)) }
        slot.sizeBytes?.let { EstateRow("Size", "${gib(it)} GiB") }
        slot.vramMb?.let { EstateRow("VRAM estimate", String.format(Locale.US, "%.0f MiB", it)) }
        slot.pid?.let { EstateRow("PID", it.toString()) }
        slot.uptimeS?.let { EstateRow("Uptime", duration(it)) }
        slot.state?.let { EstateRow("State", it) }
        slot.lastError?.let { EstateRow("Last error", it, valueColor = colors.error) }
        slot.lastTransition?.let { EstateRow("Last transition", it) }
        EstateRow("In flight", slot.inFlight.toString())
        EstateRow("Restarts", slot.restarts.toString())
        EstateRow(
            "Resident",
            when {
                slot.residentModels.isNotEmpty() -> slot.residentModels.joinToString(", ")
                slot.model != null -> slot.model
                else -> "none"
            },
        )
        slot.cgroup?.let { cgroup ->
            cgroup.currentBytes?.let { EstateRow("Memory now", "${gib(it)} GiB") }
            cgroup.highBytes?.let { EstateRow("Memory high", "${gib(it)} GiB") }
            cgroup.maxBytes?.let { EstateRow("Memory max", "${gib(it)} GiB") }
            cgroup.oomKills?.let { EstateRow("OOM kills", it.toString()) }
        }
    }
}

@Composable
private fun EstateSection(status: EstateStatus) {
    val colors = DshTheme.colors
    SectionLabel("ESTATE")
    if (status.vramUsedGib != null) {
        EstateRow("VRAM used", "${gib(status.vramUsedGib)} GiB")
    } else {
        Text(
            text = "VRAM is not reported.",
            style = DshType.bodySmall,
            color = colors.labelTertiary,
        )
    }
    EstateRow("Media in flight", status.mediaInFlight.toString())
    status.idleTimeoutS?.let { EstateRow("Idle timeout", duration(it)) }
    EstateRow(
        "Health",
        healthText(status.health),
        valueColor = if (healthOk(status.health)) colors.success else colors.warn,
    )
}

@Composable
private fun SectionLabel(text: String) {
    val colors = DshTheme.colors
    Spacer(Modifier.height(DshSpacing.md))
    Text(
        text = text,
        style = DshType.micro.copy(fontWeight = FontWeight.Medium),
        color = colors.labelCaption,
    )
}

@Composable
private fun EstateCard(content: @Composable ColumnScope.() -> Unit) {
    val colors = DshTheme.colors
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(DshRadius.card))
            .background(colors.bgLayer1)
            .padding(DshSpacing.lg),
        verticalArrangement = Arrangement.spacedBy(DshSpacing.xs),
        content = content,
    )
}

@Composable
private fun EstateRow(label: String, value: String, valueColor: Color? = null) {
    val colors = DshTheme.colors
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Text(
            text = label,
            style = DshType.bodySmall,
            color = colors.labelTertiary,
            modifier = Modifier.width(112.dp),
        )
        Text(
            text = value,
            style = DshType.bodySmall,
            color = valueColor ?: colors.labelPrimary,
            modifier = Modifier.weight(1f),
        )
    }
}

private fun idleWord(state: String?): Boolean =
    state == "idle" || state == "cold" || state == "stopped"

private fun stateWord(slot: EstateSlot): String = when (slot.state) {
    "loading" -> "loading"
    "running" -> "running"
    "ready" -> "ready"
    "failed" -> "failed"
    "idle", "cold", "stopped" -> "idle"
    else -> when {
        slot.booting -> "booting"
        slot.up -> "up"
        else -> "down"
    }
}

@Composable
private fun stateColor(slot: EstateSlot): Color {
    val colors = DshTheme.colors
    return when (slot.state) {
        "loading" -> colors.warn
        "failed" -> colors.error
        "idle", "cold", "stopped" -> colors.labelTertiary
        "running", "ready" -> colors.success
        else -> when {
            slot.booting -> colors.warn
            slot.up -> colors.success
            else -> colors.error
        }
    }
}

private fun healthOk(health: EstateHealth): Boolean =
    !health.degraded && !health.orphaned && !health.restarting

private fun healthText(health: EstateHealth): String {
    val words = ArrayList<String>()
    if (health.degraded) words += "degraded"
    if (health.orphaned) words += "orphaned"
    if (health.restarting) words += "restarting"
    val base = if (words.isEmpty()) "ok" else words.joinToString(", ")
    return if (health.restarts > 0) "$base · ${health.restarts} restarts" else base
}

private fun gib(bytes: Long): String = gib(bytes.toDouble() / (1024.0 * 1024.0 * 1024.0))

private fun gib(value: Double): String = String.format(Locale.US, "%.2f", value)

private fun duration(seconds: Double): String {
    val total = seconds.toLong().coerceAtLeast(0)
    val days = total / 86_400
    val hours = (total % 86_400) / 3_600
    val minutes = (total % 3_600) / 60
    val secs = total % 60
    return when {
        days > 0 -> "${days}d ${hours}h"
        hours > 0 -> "${hours}h ${minutes}m"
        minutes > 0 -> "${minutes}m ${secs}s"
        else -> "${secs}s"
    }
}

private fun tokenCount(tokens: Int): String =
    java.text.NumberFormat.getIntegerInstance(Locale.getDefault()).format(tokens) + " tokens"

private val estateClock = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

private fun clock(atMillis: Long): String = estateClock.format(Date(atMillis))
