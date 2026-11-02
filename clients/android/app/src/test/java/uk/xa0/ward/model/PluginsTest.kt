package uk.xa0.ward.model

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The plugin-bundle parser, its grouping and error rules, the read-only copy, and
 * the `ChangeResult` reader.
 *
 * These are pure tests of the client's half of the wire: `rpcRaw` hands back a
 * bare array, `meta` may be a string or a locale object, and the Official /
 * Installed partition is the web tab's rule rather than anything the host sends.
 * Nothing here can show that the running host answers this shape, or that its
 * built-in bundle denylist matches this client's deliberate absence of one — that
 * is a host-contract fact. The section's own test holds the composable to drawing
 * what these functions return.
 */
class PluginsTest {

    private fun bundle(
        name: String? = "@acme/alpha",
        version: String? = "1.2.3",
        enabled: Boolean = true,
        installed: Boolean = true,
        optional: Boolean = false,
        meta: Any? = null,
        readOnlyReason: String? = null,
        error: JSONObject? = null,
    ): JSONObject = JSONObject().apply {
        name?.let { put("name", it) }
        version?.let { put("version", it) }
        put("enabled", enabled)
        put("installed", installed)
        put("optional", optional)
        meta?.let { put("meta", it) }
        readOnlyReason?.let { put("readOnlyReason", it) }
        error?.let { put("error", it) }
    }

    @Test
    fun `a normal bundle array parses every bundle`() {
        val bundles = parsePluginBundles(
            JSONArray()
                .put(
                    bundle(
                        name = "@acme/alpha",
                        meta = JSONObject()
                            .put("title", "Alpha")
                            .put("description", "Does alpha things."),
                    ),
                )
                .put(
                    bundle(
                        name = "@acme/beta",
                        version = null,
                        enabled = false,
                        installed = false,
                    ),
                ),
        )

        assertEquals(2, bundles.size)
        assertEquals("@acme/alpha", bundles[0].name)
        assertEquals("1.2.3", bundles[0].version)
        assertTrue(bundles[0].enabled)
        assertTrue(bundles[0].installed)
        assertEquals("@acme/beta", bundles[1].name)
        assertNull(bundles[1].version)
        assertFalse(bundles[1].enabled)
        assertFalse(bundles[1].installed)
    }

    /**
     * `rpc` types its answer as an object, which is why the read goes through
     * `rpcRaw`. An object here is a malformed answer, not a deployment with no
     * bundles; the caller distinguishes the two with [isPluginBundleAnswer].
     */
    @Test
    fun `a non-array answer is not a bundle list`() {
        assertEquals(emptyList<PluginBundle>(), parsePluginBundles(JSONObject()))
        assertEquals(emptyList<PluginBundle>(), parsePluginBundles(null))
        assertFalse(isPluginBundleAnswer(JSONObject().put("bundles", JSONArray())))
        assertTrue(isPluginBundleAnswer(JSONArray()))
    }

    @Test
    fun `meta title and description read as plain strings`() {
        val bundles = parsePluginBundles(
            JSONArray().put(
                bundle(
                    meta = JSONObject()
                        .put("title", "Alpha")
                        .put("description", "Does alpha things."),
                ),
            ),
        )

        assertEquals("Alpha", bundles.single().title)
        assertEquals("Does alpha things.", bundles.single().description)
    }

    @Test
    fun `a locale meta takes en first`() {
        val bundles = parsePluginBundles(
            JSONArray().put(
                bundle(
                    meta = JSONObject()
                        .put("title", JSONObject().put("fi", "Alfa").put("en", "Alpha"))
                        .put(
                            "description",
                            JSONObject().put("en", "Does alpha things.").put("fi", "Tekee alfa-asioita."),
                        ),
                ),
            ),
        )

        assertEquals("Alpha", bundles.single().title)
        assertEquals("Does alpha things.", bundles.single().description)
    }

    /**
     * Without `en`, the first string in the host's own key order wins. org.json
     * keeps an object's insertion order, so this is the host's order rather than
     * the client's.
     */
    @Test
    fun `a locale meta without en takes its first string`() {
        val bundles = parsePluginBundles(
            JSONArray().put(
                bundle(
                    meta = JSONObject()
                        .put("title", JSONObject().put("fi", "Alfa").put("sv", "Alfa SV"))
                        .put(
                            "description",
                            JSONObject().put("sv", "Alfa beskrivning").put("fi", "Alfa kuvaus"),
                        ),
                ),
            ),
        )

        assertEquals("Alfa", bundles.single().title)
        assertEquals("Alfa beskrivning", bundles.single().description)
    }

    @Test
    fun `missing or blank meta falls back to the package name and leaves the description empty`() {
        val bundles = parsePluginBundles(
            JSONArray()
                .put(bundle(name = "@acme/alpha"))
                .put(
                    bundle(
                        name = "@acme/beta",
                        meta = JSONObject()
                            .put("title", "   ")
                            .put("description", JSONObject()),
                    ),
                ),
        )

        assertEquals("@acme/alpha", bundles[0].title)
        assertNull(bundles[0].description)
        assertEquals("@acme/beta", bundles[1].title)
        assertNull(bundles[1].description)
    }

    /**
     * `name` is the only handle `setBundleEnabled` has, so an invented one could
     * address the wrong bundle. A bundle the host cannot name is dropped and the
     * remaining rows still parse.
     */
    @Test
    fun `a blank or missing name is skipped`() {
        val bundles = parsePluginBundles(
            JSONArray()
                .put(bundle(name = null))
                .put(bundle(name = "   "))
                .put(bundle(name = ""))
                .put(JSONObject().put("title", "nameless"))
                .put("not an object")
                .put(bundle(name = "@acme/kept")),
        )

        assertEquals(1, bundles.size)
        assertEquals("@acme/kept", bundles.single().name)
    }

    @Test
    fun `a bundle carries its flags version and read only reason`() {
        val bundle = parsePluginBundles(
            JSONArray().put(
                bundle(
                    name = "@acme/alpha",
                    version = "2.0.0",
                    enabled = false,
                    installed = true,
                    optional = true,
                    readOnlyReason = "management-required",
                    meta = JSONObject().put("title", "Alpha"),
                ),
            ),
        ).single()

        assertEquals("@acme/alpha", bundle.name)
        assertEquals("2.0.0", bundle.version)
        assertFalse(bundle.enabled)
        assertTrue(bundle.installed)
        assertTrue(bundle.optional)
        assertEquals("management-required", bundle.readOnlyReason)
        assertTrue(bundle.readOnly)
    }

    @Test
    fun `official is an optional bundle the profile has not installed`() {
        fun officialOf(
            installed: Boolean,
            optional: Boolean,
        ): Boolean = parsePluginBundles(JSONArray().put(bundle(installed = installed, optional = optional)))
            .single()
            .official

        assertTrue(officialOf(installed = false, optional = true))
        assertFalse(officialOf(installed = true, optional = true))
        assertFalse(officialOf(installed = false, optional = false))
    }

    @Test
    fun `listed keeps installed optional and errored bundles`() {
        fun listedOf(
            installed: Boolean = false,
            optional: Boolean = false,
            error: JSONObject? = null,
        ): Boolean = parsePluginBundles(
            JSONArray().put(bundle(installed = installed, optional = optional, error = error)),
        ).single().listed

        assertTrue(listedOf(installed = true))
        assertTrue(listedOf(optional = true))
        assertTrue(listedOf(error = JSONObject().put("code", "bundle/load-failed")))
        assertFalse(listedOf())
    }

    @Test
    fun `errorSummary prefers the host diagnostic`() {
        val bundle = parsePluginBundles(
            JSONArray().put(
                bundle(
                    error = JSONObject()
                        .put("code", "bundle/load-failed")
                        .put("diagnostic", "The bundle could not be loaded."),
                ),
            ),
        ).single()

        assertTrue(bundle.hasError)
        assertEquals("bundle/load-failed", bundle.errorCode)
        assertEquals("The bundle could not be loaded.", bundle.errorText)
        assertEquals("The bundle could not be loaded.", bundle.errorSummary)
    }

    /**
     * `meta.error` is shown as text but does not make the row an error row: only
     * the top-level `error` object does, which is the web tab's visibility test.
     */
    @Test
    fun `meta error is text without making the row a problem`() {
        val bundle = parsePluginBundles(
            JSONArray().put(
                bundle(meta = JSONObject().put("error", "The bundle is deprecated.")),
            ),
        ).single()

        assertFalse(bundle.hasError)
        assertEquals("The bundle is deprecated.", bundle.errorText)
        assertEquals("The bundle is deprecated.", bundle.errorSummary)
    }

    @Test
    fun `a code alone still summaries the problem`() {
        val bundle = parsePluginBundles(
            JSONArray().put(bundle(error = JSONObject().put("code", "bundle/load-failed"))),
        ).single()

        assertTrue(bundle.hasError)
        assertNull(bundle.errorText)
        assertEquals("bundle/load-failed", bundle.errorSummary)
    }

    @Test
    fun `the read only label covers both reasons and an unknown one`() {
        assertEquals(
            "This plugin bundle is managed by the deployment's configuration and cannot be switched here.",
            pluginReadOnlyReasonLabel("management-required"),
        )
        assertEquals(
            "The host cannot address this plugin bundle from here, so it cannot be switched.",
            pluginReadOnlyReasonLabel("unaddressable"),
        )
        assertEquals(
            "This plugin bundle cannot be switched from here.",
            pluginReadOnlyReasonLabel("something-new"),
        )
    }

    @Test
    fun `an applied change is accepted`() {
        val change = readPluginChange(
            JSONObject()
                .put("changed", true)
                .put("application", "applied")
                .put("target", "@acme/alpha")
                .put("enabled", true),
        )

        assertTrue(change.changed)
        assertEquals("applied", change.application)
        assertEquals("@acme/alpha", change.target)
        assertEquals(true, change.enabled)
        assertTrue(change.accepted)
        assertNull(change.failureText())
        assertEquals(emptyList<String>(), change.warnings)
    }

    /**
     * A stored change is accepted even though it is not in effect yet: the host
     * has done what was asked and is waiting for a restart, so the refusal
     * wording must not appear.
     */
    @Test
    fun `a restart-required change is accepted`() {
        val change = readPluginChange(
            JSONObject()
                .put("changed", true)
                .put("application", "restart-required")
                .put("target", "@acme/alpha")
                .put("enabled", false),
        )

        assertTrue(change.accepted)
        assertNull(change.failureText())
        assertEquals(false, change.enabled)
    }

    @Test
    fun `a failed change keeps the host diagnostic and warnings`() {
        val change = readPluginChange(
            JSONObject()
                .put("changed", true)
                .put("application", "failed")
                .put("target", "@acme/alpha")
                .put(
                    "error",
                    JSONObject()
                        .put("code", "bundle/load-failed")
                        .put("diagnostic", "The bundle could not be loaded."),
                )
                .put(
                    "warnings",
                    JSONArray()
                        .put("Restart may be required.")
                        .put(JSONObject().put("code", "deprecated"))
                        .put(JSONObject().put("message", "The bundle is untested."))
                        .put("")
                        .put(7),
                ),
        )

        assertFalse(change.accepted)
        assertEquals("failed", change.application)
        assertEquals("bundle/load-failed", change.errorCode)
        assertEquals("The bundle could not be loaded.", change.errorDiagnostic)
        assertEquals("The bundle could not be loaded.", change.failureText())
        assertEquals(
            listOf("Restart may be required.", "deprecated", "The bundle is untested."),
            change.warnings,
        )
    }

    @Test
    fun `a failed change without a diagnostic falls back to the code`() {
        val change = readPluginChange(
            JSONObject()
                .put("changed", false)
                .put("application", "failed")
                .put("target", "@acme/alpha")
                .put("error", JSONObject().put("code", "bundle/load-failed")),
        )

        assertEquals("bundle/load-failed", change.failureText())
        assertFalse(change.accepted)
    }

    /**
     * A cancellation is deliberate: the host did nothing, so it is not accepted,
     * but it is reported as a cancellation rather than as a failure.
     */
    @Test
    fun `a cancelled change is not accepted`() {
        val change = readPluginChange(
            JSONObject()
                .put("changed", false)
                .put("application", "cancelled")
                .put("target", "@acme/alpha"),
        )

        assertEquals("The host cancelled the change.", change.failureText())
        assertFalse(change.accepted)
    }
}
