package uk.xa0.ward.extension.builtin

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import uk.xa0.ward.DshApplication
import uk.xa0.ward.extension.DshExtension
import uk.xa0.ward.extension.EXTENSION_ABI_VERSION
import uk.xa0.ward.extension.ExtensionBuilder
import uk.xa0.ward.extension.ExtensionScope
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.ui.HtopPanel
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

const val HTOP_EXTENSION_ID: String = "uk.xa0.ward.builtin.htop"
const val HTOP_TAB_ID: String = "htop"

class HtopExtension : DshExtension {
    override val id: String = HTOP_EXTENSION_ID
    override val title: String = "Htop"
    override val version: Int = EXTENSION_ABI_VERSION

    override fun contribute(builder: ExtensionBuilder) {
        builder.sessionTab(
            SessionTab(
                id = HTOP_TAB_ID,
                label = "Htop",
                order = 500,
                visible = { scope -> scope.sessionId.value != null },
                content = { scope -> HtopHost(scope) },
            ),
        )
    }
}

@Composable
private fun HtopHost(scope: ExtensionScope) {
    val session by scope.sessionId.collectAsState()
    if (session == null) {
        Box(Modifier.fillMaxSize().padding(DshSpacing.xl), contentAlignment = Alignment.Center) {
            Text("Open a session to use Htop.", style = DshType.bodySmall, color = DshTheme.colors.labelTertiary)
        }
        return
    }
    val app = LocalContext.current.applicationContext as DshApplication
    HtopPanel(app.htop, session!!)
}
