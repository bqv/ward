package uk.xa0.ward

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which forwarded host events re-read the model catalog.
 *
 * The picker was stale until a full restart because the only catalog trigger was the
 * connect-time read plus one event. This pins the predicate both push events share, so
 * dropping `credentials/reference-updated` (one of the web's refresh triggers) or
 * quietly adding `settings/document-updated` (one emit per live-status tick) fails here.
 *
 * This asserts the trigger set only: no socket is opened and no catalog is read.
 */
class ModelCatalogRefreshEventTest {

    @Test
    fun providerSetChangeRefreshesTheCatalog() {
        assertTrue(isModelCatalogRefreshEvent("llm/adapters-updated"))
    }

    @Test
    fun credentialReferenceChangeRefreshesTheCatalog() {
        assertTrue(isModelCatalogRefreshEvent("credentials/reference-updated"))
    }

    @Test
    fun aSettingsWriteIsNotACatalogRefresh() {
        assertFalse(isModelCatalogRefreshEvent("settings/document-updated"))
    }

    @Test
    fun anUnrelatedEventIsNotACatalogRefresh() {
        assertFalse(isModelCatalogRefreshEvent("api-session/status"))
        assertFalse(isModelCatalogRefreshEvent(""))
    }
}
