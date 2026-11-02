package uk.xa0.ward

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import uk.xa0.ward.model.ApprovalAnswer

class ApprovalActivityPhaseTest {

    @Test
    fun `an accepted answer names the outcome the host took`() {
        assertEquals("granted", approvalActivityPhase(ApprovalAnswer.Sent(true)))
        assertEquals("rejected", approvalActivityPhase(ApprovalAnswer.Sent(false)))
    }

    @Test
    fun `a state that is not the host's acceptance names nothing`() {
        assertNull(approvalActivityPhase(ApprovalAnswer.Waiting))
        assertNull(approvalActivityPhase(ApprovalAnswer.Answering(true)))
        assertNull(approvalActivityPhase(ApprovalAnswer.Answering(false)))
        assertNull(approvalActivityPhase(ApprovalAnswer.Unsent(true, "the host refused it")))
        assertNull(approvalActivityPhase(ApprovalAnswer.Unsent(null, "the request timed out")))
    }
}
