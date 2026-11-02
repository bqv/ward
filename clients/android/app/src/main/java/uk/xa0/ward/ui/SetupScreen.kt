package uk.xa0.ward.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import uk.xa0.ward.model.StartupCopy
import uk.xa0.ward.model.StartupStep
import uk.xa0.ward.net.AcceptedCert
import uk.xa0.ward.net.CertReadout
import uk.xa0.ward.ui.components.CertificateSection
import uk.xa0.ward.ui.components.DshMark
import uk.xa0.ward.ui.components.DshPrimaryButton
import uk.xa0.ward.ui.components.DshTextField
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType
import java.security.cert.X509Certificate

/**
 * First-run / reconnect screen.
 *
 * It mirrors the DSH login page: the mark centred above a short form. The
 * credential model is the host's own — the app signs in through the same
 * `/auth/login` the web UI posts to, and never needs anything installed on the host.
 */
@Composable
fun SetupScreen(
    initialUrl: String,
    initialUsername: String,
    error: String?,
    busy: Boolean,
    /**
     * What the connect attempt is waiting on, or null when none is in flight.
     *
     * A step rather than a sentence for the same reason the loading screen takes
     * one: this line used to be whatever the event multiplexer last wrote, so a
     * reader connecting from here could be told `item s2 baseline (#1)`.
     */
    startupStep: StartupStep?,
    onConnect: (url: String, lanUrl: String, username: String, password: String, cookie: String) -> Unit,
    initialLanUrl: String = "",
    /**
     * The hosts this app already trusts, one certificate each.
     *
     * Passed in rather than fetched so the screen can say "pinned, since when, with
     * which fingerprint" before anything is dialled — which is the moment at which
     * the answer still matters.
     */
    acceptedCerts: List<AcceptedCert> = emptyList(),
    /** What the last certificate read saw, from a check or from a refusal. */
    readout: CertReadout? = null,
    checking: Boolean = false,
    certError: String? = null,
    onCheckCertificate: (String) -> Unit = {},
    onTrustCertificate: (String, X509Certificate) -> Unit = { _, _ -> },
    onForgetCertificate: (String) -> Unit = {},
) {
    val colors = DshTheme.colors
    var url by rememberSaveable { mutableStateOf(initialUrl.ifBlank { "tcp://127.0.0.1:45872" }) }
    var lanUrl by rememberSaveable { mutableStateOf(initialLanUrl) }
    var username by rememberSaveable { mutableStateOf(initialUsername) }
    var password by rememberSaveable { mutableStateOf("") }
    var cookie by rememberSaveable { mutableStateOf("") }
    var advanced by rememberSaveable { mutableStateOf(false) }

    // One sign-in is one request. The latch is written synchronously by the tap
    // or by the IME's Done, so a second event in the same frame — a double tap,
    // or Done followed by a tap — is dropped before it can post the same
    // credentials a second time. A second request in flight is exactly how a
    // user adds a strike to their own account. The latch is released when the
    // attempt ends: `busy` returns to false on a failure, and the screen has to
    // be usable again.
    var submitting by remember { mutableStateOf(false) }
    LaunchedEffect(busy) { if (!busy) submitting = false }
    val submit: () -> Unit = {
        if (!submitting && !busy && url.isNotBlank()) {
            submitting = true
            onConnect(url, lanUrl, username, password, cookie)
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = DshSpacing.xxl, vertical = DshSpacing.xxxl),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Spacer(Modifier.height(48.dp))
        DshMark(size = 44.dp)
        Spacer(Modifier.height(DshSpacing.xl))
        Text("Ward", style = DshType.heading1, color = colors.ink)
        Spacer(Modifier.height(DshSpacing.md))
        Text(
            text = "Connect to your ward daemon",
            style = DshType.bodyMedium,
            color = colors.labelTertiary,
        )
        Spacer(Modifier.height(DshSpacing.xxxl))

        Column(Modifier.widthIn(max = 420.dp), verticalArrangement = Arrangement.spacedBy(DshSpacing.xl)) {
            DshTextField(
                value = url,
                onValueChange = { url = it },
                placeholder = "tcp://127.0.0.1:45872",
                label = "Daemon",
                keyboardType = KeyboardType.Uri,
                imeAction = ImeAction.Done,
                onImeDone = submit,
            )
            Text(
                text = "Use the loopback TCP door ward prints, forwarded to this phone with " +
                    "adb reverse tcp:45872 tcp:45872. A plain host:port is accepted too.",
                style = DshType.micro,
                color = colors.labelCaption,
            )

            if (error != null) {
                Text(
                    text = error,
                    style = DshType.bodySmall,
                    color = colors.error,
                )
            }

            DshPrimaryButton(
                text = when {
                    busy -> startupStep?.let { StartupCopy.title(it) } ?: "Connecting…"
                    else -> "Connect"
                },
                enabled = !busy && !submitting && url.isNotBlank(),
                onClick = submit,
            )

            if (busy) {
                Text(
                    text = startupStep?.let { StartupCopy.detail(it) }.orEmpty(),
                    style = DshType.micro,
                    color = colors.labelCaption,
                )
            }

            Spacer(Modifier.height(DshSpacing.lg))
            Text(
                text = "Ward speaks its own binary CBOR protocol over a socket. " +
                    "Nothing is installed on the host and no web UI is involved.",
                style = DshType.micro,
                color = colors.labelCaption,
            )
        }
        Spacer(Modifier.height(48.dp))
    }
}

