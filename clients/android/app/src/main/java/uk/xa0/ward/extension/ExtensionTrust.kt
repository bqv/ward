package uk.xa0.ward.extension

import java.security.MessageDigest

/**
 * The certificate trust store for runtime-loaded extension APKs.
 *
 * Android's normal package installer compares signatures on update for an
 * installed package. An APK in this app's private storage was never installed, so
 * the platform never made that comparison for us: the pin here is what stands in
 * its place. First use pins the signing certificate's SHA-256 (TOFU); every later
 * install of the same package must present the same fingerprint. A mismatch is a
 * refusal, not a prompt — replacing a trusted extension's signer is exactly the
 * move the pin is meant to catch.
 *
 * The store is a package name → fingerprint map, persisted in `ConfigStore` beside
 * the other client-local trust anchors. The functions are pure so the pin and
 * mismatch paths are JVM-testable.
 */
object ExtensionTrust {

    /** What a candidate APK's certificate is, relative to the pins. */
    sealed interface Decision {
        /** Nothing is pinned for this package; installing will pin this cert. */
        data object FirstUse : Decision

        /** The package is pinned to this exact certificate. */
        data object Pinned : Decision

        /** A different certificate is pinned; this APK must be refused. */
        data class Mismatch(val pinnedFingerprint: String) : Decision
    }

    /** A malformed fingerprint, package name or attempted update. */
    class ExtensionTrustException(message: String) : Exception(message)

    /** A package name that is safe to use as one directory component. */
    private val PACKAGE = Regex("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$")

    private val HEX = Regex("^[0-9a-f]{64}$")

    /** SHA-256 of the certificate's DER bytes, lowercase hex, no separators. */
    fun fingerprintSha256(certificateDer: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(certificateDer)
        return digest.joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    /** Canonical storage form: lowercase hex with any `:`/whitespace removed. */
    fun normalise(fingerprint: String): String =
        fingerprint.filter { it.isLetterOrDigit() }.lowercase()

    fun isWellFormedFingerprint(fingerprint: String): Boolean =
        HEX.matches(normalise(fingerprint))

    fun isWellFormedPackage(packageName: String): Boolean =
        PACKAGE.matches(packageName)

    /** `AB:CD:…` grouping, for a prompt a person can compare with another reader. */
    fun display(fingerprint: String): String =
        normalise(fingerprint)
            .chunked(2)
            .joinToString(":")
            .uppercase()

    fun decision(
        pins: Map<String, String>,
        packageName: String,
        fingerprint: String,
    ): Decision {
        val pinned = pins[packageName] ?: return Decision.FirstUse
        return if (normalise(pinned) == normalise(fingerprint)) {
            Decision.Pinned
        } else {
            Decision.Mismatch(pinned)
        }
    }

    /**
     * The store with this certificate pinned for this package.
     *
     * Returns the same map for an already-matching pin, adds a new one on first
     * use, and throws on a mismatch. A refusal here is deliberate: an update that
     * changes signer is not a decision a dialog can make safe.
     */
    fun pin(
        pins: Map<String, String>,
        packageName: String,
        fingerprint: String,
    ): Map<String, String> {
        if (!isWellFormedPackage(packageName)) {
            throw ExtensionTrustException("“$packageName” is not a valid package name.")
        }
        val canonical = normalise(fingerprint)
        if (!isWellFormedFingerprint(canonical)) {
            throw ExtensionTrustException("The signing certificate fingerprint is malformed.")
        }
        return when (val decision = decision(pins, packageName, canonical)) {
            Decision.FirstUse -> pins + (packageName to canonical)
            Decision.Pinned -> pins
            is Decision.Mismatch -> throw ExtensionTrustException(
                "“$packageName” is pinned to a different signing certificate " +
                    "(${display(decision.pinnedFingerprint)}); this APK is signed with " +
                    "${display(canonical)}.",
            )
        }
    }

    /**
     * Line-oriented persistence: `package=hex`, sorted by package. Package names
     * and lowercase hex contain neither `=` nor newlines, so no escaping is
     * needed; a malformed line is dropped rather than allowed to poison the store.
     */
    fun encode(pins: Map<String, String>): String =
        pins.entries
            .asSequence()
            .filter { isWellFormedPackage(it.key) && isWellFormedFingerprint(it.value) }
            .sortedBy { it.key }
            .joinToString("\n") { "${it.key}=${normalise(it.value)}" }

    fun decode(raw: String?): Map<String, String> {
        if (raw.isNullOrBlank()) return emptyMap()
        val pins = LinkedHashMap<String, String>()
        raw.lineSequence().forEach { line ->
            val separator = line.indexOf('=')
            if (separator <= 0) return@forEach
            val packageName = line.substring(0, separator).trim()
            val fingerprint = normalise(line.substring(separator + 1))
            if (isWellFormedPackage(packageName) && isWellFormedFingerprint(fingerprint)) {
                pins[packageName] = fingerprint
            }
        }
        return pins
    }
}
