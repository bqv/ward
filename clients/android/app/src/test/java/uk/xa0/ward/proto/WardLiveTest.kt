/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

package uk.xa0.ward.proto

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
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
            assertTrue(api.listSessions().any { it.id == session.id })

            val subscription = api.resume(session.id, 0)
            assertEquals(session.id, subscription.session?.id)
            val renamed = api.rename(session.id, "ward-android smoke")
            assertEquals("ward-android smoke", renamed.name)
            val event = withTimeout(5_000) { subscription.events.first() }
            assertTrue(event.kind.isNotEmpty())
            subscription.close()

            val temp = java.nio.file.Files.createTempFile(java.nio.file.Path.of("/tmp"), "ward-android", ".txt")
            java.nio.file.Files.write(temp, "ward file browser\n".toByteArray())
            try {
                assertTrue(api.listFiles(session.id, "").array("entries")?.items?.isNotEmpty() == true)
                assertEquals("ward file browser\n", api.readFileText(session.id, temp.fileName.toString()).text("text"))
                assertEquals("ward ", api.readFileBlob(session.id, temp.fileName.toString(), 0, 5).bytes("data")?.toString(Charsets.UTF_8))
            } finally {
                java.nio.file.Files.deleteIfExists(temp)
            }
            assertTrue(api.listApprovals().isEmpty())

            assertTrue(api.archive(session.id, true).archived)
        } finally {
            connection.close()
        }
    }
    @Test
    fun `a parked write is listed and answerable through approval dot list`() = runBlocking {
        val endpoint = System.getenv("WARD_TEST_ENDPOINT") ?: return@runBlocking
        val owner = WardConnection(endpoint)
        val worker = WardConnection(endpoint)
        owner.connect()
        worker.connect()
        val ownerApi = WardApi(owner)
        val workerApi = WardApi(worker)
        val session = ownerApi.createSession("/tmp")
        val prompt = async(Dispatchers.IO) {
            workerApi.prompt(session.id, "/write /tmp/ward-android-approval.txt hello")
        }
        try {
            var pending = emptyList<Cbor.Map>()
            for (attempt in 0 until 40) {
                pending = ownerApi.listApprovals()
                if (pending.isNotEmpty()) break
                delay(100)
            }
            val held = pending.firstOrNull { it.text("session") == session.id }
                ?: error("the parked write was not listed: $pending")
            assertEquals("fs.write", held.text("tool"))
            ownerApi.respondApproval(
                session.id,
                held.long("call") ?: error("no call id"),
                held.long("epoch") ?: error("no epoch"),
                false,
            )
            withTimeout(10_000) { prompt.join() }
            assertTrue(ownerApi.listApprovals().none { it.text("session") == session.id })
        } finally {
            runCatching { ownerApi.cancel(session.id) }
            prompt.cancel()
            owner.close()
            worker.close()
        }
    }

}
