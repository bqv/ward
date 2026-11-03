/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.term

import java.util.concurrent.atomic.AtomicLong

const val HTOP_TERMINAL_PREFIX: String = "ward-htop-"
const val HTOP_COMMAND: String = "exec htop\n"

private val htopSequence = AtomicLong(System.currentTimeMillis())

fun htopTerminalId(): String = HTOP_TERMINAL_PREFIX + htopSequence.incrementAndGet().toString(36)

enum class HtopPhase { IDLE, STARTING, CONNECTING, RUNNING, DISCONNECTED, FAILED }

data class HtopState(
    val phase: HtopPhase = HtopPhase.IDLE,
    val writable: Boolean = false,
    val error: String? = null,
    val revision: Long = 0L,
    val terminalId: String = "",
)

fun htopPhaseWord(phase: HtopPhase): String = when (phase) {
    HtopPhase.IDLE -> ""
    HtopPhase.STARTING -> "starting"
    HtopPhase.CONNECTING -> "connecting"
    HtopPhase.RUNNING -> "running"
    HtopPhase.DISCONNECTED -> "detached"
    HtopPhase.FAILED -> "failed"
}

fun htopTerminalToAdopt(retained: List<TerminalInfo>): TerminalInfo? =
    retained.firstOrNull { it.id.startsWith(HTOP_TERMINAL_PREFIX) && it.state == TerminalState.RUNNING }

fun htopTerminalsToRetire(retained: List<TerminalInfo>): List<TerminalInfo> =
    retained.filter { it.id.startsWith(HTOP_TERMINAL_PREFIX) && it.state != TerminalState.RUNNING }
