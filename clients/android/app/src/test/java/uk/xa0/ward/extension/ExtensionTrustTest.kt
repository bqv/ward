package uk.xa0.ward.extension

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The pin rules that stand in for Android's package installer.
 *
 * The archive half — reading a real APK's signing certificate through
 * `PackageManager` — is Android and is not in this test; a JVM cannot parse an APK
 * the way the platform does. What *is* here is the whole trust decision: the
 * first-use pin, a matching update, a different signer refused, and a store that
 * round-trips through `ConfigStore`'s line encoding.
 */
class ExtensionTrustTest {

    private val certA = "hello".toByteArray()
    private val certB = "goodbye".toByteArray()

    private val fingerprintA = ExtensionTrust.fingerprintSha256(certA)
    private val fingerprintB = ExtensionTrust.fingerprintSha256(certB)

    @Test
    fun `a fingerprint is sha256 lowercase hex`() {
        // Known SHA-256 of "hello"; checks the app and any other tool agree.
        assertEquals(
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
            fingerprintA,
        )
        assertTrue(ExtensionTrust.isWellFormedFingerprint(fingerprintA))
        assertFalse(ExtensionTrust.isWellFormedFingerprint("not a fingerprint"))
    }

    @Test
    fun `a prompt fingerprint is grouped for reading`() {
        assertEquals(
            "2C:F2:4D:BA:5F:B0:A3:0E:26:E8:3B:2A:C5:B9:E2:9E:" +
                "1B:16:1E:5C:1F:A7:42:5E:73:04:33:62:93:8B:98:24",
            ExtensionTrust.display(fingerprintA),
        )
    }

    @Test
    fun `first use pins the certificate and a later match is pinned`() {
        val packageName = "com.example.extension"

        assertEquals(
            ExtensionTrust.Decision.FirstUse,
            ExtensionTrust.decision(emptyMap(), packageName, fingerprintA),
        )
        val pinned = ExtensionTrust.pin(emptyMap(), packageName, fingerprintA)

        assertEquals(fingerprintA, pinned[packageName])
        assertEquals(
            ExtensionTrust.Decision.Pinned,
            ExtensionTrust.decision(pinned, packageName, fingerprintA),
        )
    }

    @Test
    fun `an update with a different signer is refused`() {
        val packageName = "com.example.extension"
        val pinned = ExtensionTrust.pin(emptyMap(), packageName, fingerprintA)

        val decision = ExtensionTrust.decision(pinned, packageName, fingerprintB)

        assertTrue(decision is ExtensionTrust.Decision.Mismatch)
        assertEquals(
            fingerprintA,
            (decision as ExtensionTrust.Decision.Mismatch).pinnedFingerprint,
        )
        val failure = assertThrows(ExtensionTrust.ExtensionTrustException::class.java) {
            ExtensionTrust.pin(pinned, packageName, fingerprintB)
        }
        assertTrue(failure.message.orEmpty().contains("pinned to a different signing certificate"))
    }

    @Test
    fun `pinning an unchanged certificate does not disturb the store`() {
        val pins = mapOf("com.example.one" to fingerprintA, "com.example.two" to fingerprintB)

        val again = ExtensionTrust.pin(pins, "com.example.one", fingerprintA)

        assertEquals(pins, again)
    }

    @Test
    fun `the store round-trips and drops malformed lines`() {
        val pins = mapOf(
            "com.example.two" to fingerprintB,
            "com.example.one" to fingerprintA,
        )

        val encoded = ExtensionTrust.encode(pins)
        assertEquals(encoded, ExtensionTrust.encode(pins))
        assertEquals(pins, ExtensionTrust.decode(encoded))

        val withJunk = encoded + "\nnot-a-line\ncom.example.bad=xyz"
        assertEquals(pins, ExtensionTrust.decode(withJunk))
        assertEquals(emptyMap<String, String>(), ExtensionTrust.decode(null))
    }

    @Test
    fun `a package name that could escape private storage is refused`() {
        val failure = assertThrows(ExtensionTrust.ExtensionTrustException::class.java) {
            ExtensionTrust.pin(emptyMap(), "../../etc", fingerprintA)
        }
        assertTrue(failure.message.orEmpty().contains("not a valid package name"))
        assertFalse(ExtensionTrust.isWellFormedPackage("com.example/one"))
    }
}
