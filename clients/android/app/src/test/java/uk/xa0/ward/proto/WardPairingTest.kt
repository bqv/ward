package uk.xa0.ward.proto

import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.nio.file.Files
import java.nio.file.Path
import java.security.KeyPairGenerator
import java.security.spec.ECGenParameterSpec

class WardPairingTest {
    @Test
    fun `the generated request verifies with openssl`() {
        val openssl = listOf("/usr/bin/openssl", "/bin/openssl")
            .map(Path::of)
            .firstOrNull(Files::isExecutable)
        assumeTrue("openssl is not installed on this runner", openssl != null)

        val keyPair = KeyPairGenerator.getInstance("EC").apply {
            initialize(ECGenParameterSpec("secp256r1"))
        }.generateKeyPair()
        val pem = WardPairing.csrPem(keyPair)
        val file = Files.createTempFile("ward-android", ".csr")
        try {
            Files.write(file, pem.toByteArray())
            val process = ProcessBuilder(
                openssl!!.toString(), "req", "-in", file.toString(), "-noout", "-verify",
            ).redirectErrorStream(true).start()
            val output = process.inputStream.bufferedReader().readText()
            assertEquals(output, 0, process.waitFor())
        } finally {
            Files.deleteIfExists(file)
        }
    }
}
