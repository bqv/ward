/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A real daemon on the other end of the socket.
 *
 * The golden transcript proves the bytes; this proves the client's reader,
 * request ledger and subscription plumbing against a live `ward up`. It is
 * skipped unless the harness names a door, so the ordinary suite stays hermetic:
 *
 *   WARD_TEST_ENDPOINT=tcp://127.0.0.1:45872 ./build.sh :app:testDebugUnitTest
 */
class WardLiveTest {

    @Test
    fun `create list resume and rename against a live daemon`() = runBlocking {
        val endpoint = System.getenv("WARD_TEST_ENDPOINT") ?: return@runBlocking
        val connection = WardConnection(endpoint)
        connection.connect()
        try {
            val api = WardApi(connection)
            val session = api.createSession("/tmp")
            assertTrue("session.create must mint an id", session.id.isNotEmpty())
            val raw = connection.call("session.list", cborMap("limit" to 10.cbor()))
            println("raw=$raw")
            println("sessions=" + raw.get("sessions"))
            println("parsed=" + WardSessionParser.list(raw).map { it.id })
            val listed = api.listSessions()
            println("created=${session.id} listed=" + listed.map { it.id })
            assertTrue(listed.any { it.id == session.id })

            val subscription = api.resume(session.id, 0)
            assertEquals(session.id, subscription.session?.id)
            val renamed = api.rename(session.id, "ward-android smoke")
            assertEquals("ward-android smoke", renamed.name)
            val event = withTimeout(5_000) { subscription.events.first() }
            assertTrue(event.kind.isNotEmpty())
            subscription.close()

            assertTrue(api.archive(session.id, true).archived)
        } finally {
            connection.close()
        }
    }
}
