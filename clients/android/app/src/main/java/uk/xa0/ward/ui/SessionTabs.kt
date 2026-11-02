package uk.xa0.ward.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import uk.xa0.ward.extension.ExtensionScope
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

/** One seat in the session view strip: a built-in view or an extension tab. */
internal data class SessionTabItem(
    val key: String,
    val label: String,
)

/**
 * The session view strip, shared by the built-in views and extension tabs.
 *
 * The web's active state is a 2px underline plus `state-business-primary` text,
 * not a pill; inactive labels are `label-tertiary` at 13px/16px weight 500. The
 * strip is `gap:36px; margin-top:10px; padding-left:8px`. 36px gaps would push
 * the second label off a narrow phone once both words are drawn, so the gap is
 * tightened — the only deliberate divergence, and it costs nothing to read.
 *
 * Built-in labels and extension labels are drawn by the same function, in the
 * order the caller passes. `ChatScreen` passes the built-ins first and then the
 * extension tabs whose own `visible` predicate admitted the live scope (already
 * sorted by `order`, then id), so an extension cannot insert itself between Chat
 * and Shell.
 */
@Composable
internal fun SessionTabs(
    selectedKey: String,
    tabs: List<SessionTabItem>,
    onSelect: (String) -> Unit,
) {
    val colors = DshTheme.colors
    // The built-in three are Chat / Trajectory / Shell; the Router seat is now an
    // extension tab and is drawn only while the selected route is local, so the
    // fourth label appears and leaves with the route. `Trajectory` is the width
    // that decides a narrow phone, and the gap is tightened only when a fourth tab
    // is actually drawn, so the ordinary three-tab strip is untouched.
    val gap = if (tabs.size > 3) DshSpacing.xl else DshSpacing.xxl
    // Extension tabs append to the built-ins, and more than three labels would run
    // off a narrow phone. The strip scrolls horizontally so every seat stays
    // reachable; with only the built-in three drawn nothing about the strip's
    // appearance changes.
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(start = DshSpacing.md, end = DshSpacing.md, top = DshSpacing.md),
        horizontalArrangement = Arrangement.spacedBy(gap),
    ) {
        tabs.forEach { entry ->
            val active = entry.key == selectedKey
            // `IntrinsicSize.Max` is what keeps the underline as wide as the label:
            // a plain `fillMaxWidth()` inside the Row measures against the *row*,
            // so the first tab would stretch across the strip and push the second
            // one off the screen.
            Column(
                Modifier
                    .width(IntrinsicSize.Max)
                    .clip(RoundedCornerShape(topStart = 4.dp, topEnd = 4.dp))
                    .clickableNoRipple { onSelect(entry.key) }
                    .padding(horizontal = DshSpacing.xs, vertical = DshSpacing.md),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = entry.label,
                    style = DshType.micro.copy(fontSize = 13.sp, lineHeight = 16.sp, fontWeight = FontWeight.Medium),
                    color = if (active) colors.link else colors.labelTertiary,
                )
                Spacer(Modifier.height(DshSpacing.md))
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(2.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(if (active) colors.link else Color.Transparent),
                )
            }
        }
    }
}

/**
 * The body of an extension tab.
 *
 * A box rather than a column: the extension owns its own layout inside the seat,
 * and the host still owns where the seat is. The scope is the only surface the
 * content is given.
 */
@Composable
internal fun ExtensionTabContent(
    tab: SessionTab,
    scope: ExtensionScope,
    modifier: Modifier = Modifier,
) {
    Box(modifier.fillMaxSize()) {
        tab.content(scope)
    }
}
