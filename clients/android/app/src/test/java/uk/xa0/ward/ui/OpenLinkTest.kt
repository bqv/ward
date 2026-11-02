package uk.xa0.ward.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OpenLinkTest {

    @Test
    fun `http links are web links`() {
        assertTrue(isWebLink("http://example.com"))
        assertTrue(isWebLink("http://example.com:8080/path?q=1#frag"))
        assertTrue(isWebLink("http://localhost"))
    }

    @Test
    fun `https links are web links`() {
        assertTrue(isWebLink("https://ko-fi.com/bqvdev"))
        assertTrue(isWebLink("HTTPS://EXAMPLE.COM/Path"))
    }

    @Test
    fun `mailto is not a web link`() {
        assertFalse(isWebLink("mailto:someone@example.com"))
    }

    @Test
    fun `relative links are not web links`() {
        assertFalse(isWebLink("/docs/index.html"))
        assertFalse(isWebLink("docs/index.html"))
        assertFalse(isWebLink("#section"))
        assertFalse(isWebLink("//example.com/path"))
    }

    @Test
    fun `blank is not a web link`() {
        assertFalse(isWebLink(""))
        assertFalse(isWebLink("   "))
    }

    @Test
    fun `malformed links are not web links`() {
        assertFalse(isWebLink("http://"))
        assertFalse(isWebLink("http:///path"))
        assertFalse(isWebLink("https://exa mple.com"))
        assertFalse(isWebLink("https://[::1"))
    }

    @Test
    fun `other schemes are not web links`() {
        assertFalse(isWebLink("javascript:alert(1)"))
        assertFalse(isWebLink("intent://scan/#Intent;scheme=zxing;end"))
        assertFalse(isWebLink("tel:+358401234567"))
    }
}
