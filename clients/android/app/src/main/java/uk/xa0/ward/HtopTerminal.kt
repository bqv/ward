/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import uk.xa0.ward.data.ConfigStore
import uk.xa0.ward.net.DshClient
import uk.xa0.ward.net.StreamEvent
import uk.xa0.ward.net.TerminalClient
import uk.xa0.ward.term.HTOP_COMMAND
import uk.xa0.ward.term.HtopPhase
import uk.xa0.ward.term.HtopState
import uk.xa0.ward.term.TerminalAttachmentSession
import uk.xa0.ward.term.TerminalCellSize
import uk.xa0.ward.term.TerminalEmulator
import uk.xa0.ward.term.TerminalFrameAction
import uk.xa0.ward.term.TerminalState
import uk.xa0.ward.term.htopTerminalId
import uk.xa0.ward.term.htopTerminalToAdopt
import uk.xa0.ward.term.htopTerminalsToRetire

/**
 * A shared Htop seat for the open session.
 *
 * dsh's version owns a dedicated archived host-shell session. Ward has no
 * host-shell seat, so this one runs inside the session the reader is looking at:
 * it opens (or adopts) one `ward-htop-` terminal there, sends `exec htop`, and
 * closes over the same remote terminal frames the Shell tab uses.
 */
class HtopTerminal(
    private val client: DshClient,
    private val configStore: ConfigStore,
    private val scope: CoroutineScope,
) {
    private val terminalClient = TerminalClient(client)

    private val _state = MutableStateFlow(HtopState())
    val state: StateFlow<HtopState> = _state.asStateFlow()

    private val _font = MutableStateFlow(TerminalCellSize.DEFAULT_FONT_SP)
    val font: StateFlow<Float> = _font.asStateFlow()

    private var emulator: TerminalEmulator? = null
    private var attachment: TerminalAttachmentSession? = null
    private var sessionId: String? = null
    private var terminalId: String? = null
    private var job: Job? = null
    private var launched = false

    fun screen(): TerminalEmulator? = emulator

    fun start(session: String) {
        if (session.isBlank()) return
        if (session == sessionId && job?.isActive == true) return
        job?.cancel()
        sessionId = session
        terminalId = null
        emulator = null
        attachment = null
        launched = false
        _state.value = HtopState(phase = HtopPhase.STARTING)
        job = scope.launch { run(session) }
    }

    fun retry() {
        val current = sessionId ?: return
        start(current)
    }

    fun setFont(fontSp: Float) {
        val size = TerminalCellSize.of(fontSp)
        if (size.fontSp == _font.value) return
        _font.value = size.fontSp
        configStore.save(configStore.load().copy(terminalFontSp = size.fontSp))
    }

    fun write(data: String) {
        if (data.isEmpty()) return
        val session = attachment ?: return
        val agent = sessionId ?: return
        val id = terminalId ?: return
        if (!session.writable || !_state.value.writable) return
        scope.launch {
            runCatching { terminalClient.write(agent, id, session.attachmentId, data) }
        }
    }

    fun resize(columns: Int, rows: Int) {
        val session = attachment ?: return
        val agent = sessionId ?: return
        val id = terminalId ?: return
        if (!session.writable) return
        val grid = session.measure(columns, rows) ?: return
        scope.launch {
            runCatching { terminalClient.resize(agent, id, session.attachmentId, grid.first, grid.second) }
        }
    }

    private suspend fun run(session: String) {
        try {
            val environment = terminalClient.environment(session)
            val retained = terminalClient.list(session).filter { it.id.startsWith("ward-htop-") }
            val info = htopTerminalToAdopt(retained) ?: run {
                for (stale in htopTerminalsToRetire(retained)) {
                    runCatching { terminalClient.close(session, stale.id) }
                }
                terminalClient.create(
                    agentId = session,
                    id = htopTerminalId(),
                    columns = minOf(80, environment.maxCols),
                    rows = minOf(24, environment.maxRows),
                    shellPath = null,
                )
            }
            terminalId = info.id
            val screen = TerminalEmulator(info.cols, info.rows, environment.scrollback)
            emulator = screen
            val attached = TerminalAttachmentSession(screen, environment, info)
            attachment = attached
            _state.value = HtopState(
                phase = HtopPhase.CONNECTING,
                writable = attached.writable,
                revision = _state.value.revision + 1,
                terminalId = info.id,
            )
            terminalClient.follow(session, info.id, attached.attachmentId).collect { event ->
                when (event) {
                    is StreamEvent.Item -> {
                        val frame = TerminalClient.frameOf(event.value) ?: return@collect
                        when (val action = attached.accept(frame)) {
                            is TerminalFrameAction.Repaint -> {
                                screen.append(action.screen)
                                if (!launched) {
                                    launched = true
                                    runCatching {
                                        terminalClient.write(session, info.id, attached.attachmentId, HTOP_COMMAND)
                                    }
                                }
                                _state.value = _state.value.copy(
                                    phase = HtopPhase.RUNNING,
                                    writable = attached.writable,
                                    revision = _state.value.revision + 1,
                                )
                            }
                            TerminalFrameAction.Output -> {
                                _state.value = _state.value.copy(revision = _state.value.revision + 1)
                            }
                            is TerminalFrameAction.Metadata -> {
                                _state.value = _state.value.copy(
                                    phase = if (action.info.state == TerminalState.RUNNING) {
                                        HtopPhase.RUNNING
                                    } else {
                                        HtopPhase.DISCONNECTED
                                    },
                                    writable = attached.writable,
                                    revision = _state.value.revision + 1,
                                )
                            }
                        }
                    }
                    is StreamEvent.Failure -> {
                        _state.value = _state.value.copy(
                            phase = HtopPhase.FAILED,
                            writable = false,
                            error = event.message,
                        )
                        return@collect
                    }
                    StreamEvent.End -> Unit
                }
            }
            if (scope.isActive) {
                _state.value = _state.value.copy(phase = HtopPhase.DISCONNECTED, writable = false)
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Throwable) {
            _state.value = _state.value.copy(
                phase = HtopPhase.FAILED,
                writable = false,
                error = error.message?.takeIf { it.isNotBlank() }
                    ?: "The Htop terminal could not be opened.",
            )
        }
    }
}
