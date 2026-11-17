package uk.xa0.ward.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WorkspaceFilesTest {

    private fun file(name: String, size: Long? = null) =
        WorkspaceFileEntry(name, WorkspaceFileKind.FILE, size)

    private fun directory(name: String) =
        WorkspaceFileEntry(name, WorkspaceFileKind.DIRECTORY)

    private fun level(
        path: String,
        vararg entries: WorkspaceFileEntry,
        truncated: Boolean = false,
    ) = WorkspaceLevel(path, entries.toList(), truncated)

    @Test
    fun `a row carries the entry name, kind, size, path and depth`() {
        val rows = WorkspaceFiles.rowsOf(level("out", file("report.md", 2048), directory("sub")))

        assertEquals(listOf("report.md", "sub"), rows.map { it.name })
        assertEquals(listOf(WorkspaceFileKind.FILE, WorkspaceFileKind.DIRECTORY), rows.map { it.kind })
        assertEquals(2048L, rows[0].size)
        assertNull(rows[1].size)
        assertEquals(listOf("out/report.md", "out/sub"), rows.map { it.path })
        assertEquals(listOf(0, 0), rows.map { it.depth })
    }

    @Test
    fun `a row's mtime is the best-effort answer for its path`() {
        val seen = mutableListOf<String>()
        val rows = WorkspaceFiles.rowsOf(
            level("", file("a.txt"), file("b.txt")),
        ) { path ->
            seen += path
            if (path == "a.txt") 1_700_000_000_000L else null
        }

        assertEquals(1_700_000_000_000L, rows[0].mtimeMillis)
        assertNull(rows[1].mtimeMillis)
        assertEquals(listOf("a.txt", "b.txt"), seen)
    }

    @Test
    fun `a missing mtime lookup stays missing`() {
        val rows = WorkspaceFiles.rowsOf(level("", file("a.txt")))

        assertNull(rows.single().mtimeMillis)
    }

    @Test
    fun `hidden is the dot prefix`() {
        val rows = WorkspaceFiles.rowsOf(level("", file(".env"), file("notes.txt")))

        assertTrue(rows[0].hidden)
        assertFalse(rows[1].hidden)
    }

    @Test
    fun `a child path joins the listed path and the name`() {
        assertEquals("a.txt", WorkspaceFiles.childPath("", "a.txt"))
        assertEquals("out/a.txt", WorkspaceFiles.childPath("out", "a.txt"))
        assertEquals("out/sub/a.txt", WorkspaceFiles.childPath("out/sub", "a.txt"))
    }

    @Test
    fun `directories are the directory entries in host order up to the cap`() {
        val listing = level(
            "",
            directory("one"),
            file("a.txt"),
            directory("two"),
            directory("three"),
        )

        assertEquals(listOf("one", "two", "three"), WorkspaceFiles.directories(listing))
        assertEquals(listOf("one", "two"), WorkspaceFiles.directories(listing, cap = 2))
    }

    @Test
    fun `flatten puts the root rows first and each child level under them`() {
        val root = level("", directory("out"), file("README.md", 10))
        val out = level("out", file("report.md", 20))

        val flat = WorkspaceFiles.flatten(root, listOf(out))

        assertEquals(
            listOf("out", "README.md", "report.md"),
            flat.rows.map { it.name },
        )
        assertEquals(listOf(0, 0, 1), flat.rows.map { it.depth })
        assertEquals("out/report.md", flat.rows[2].path)
        assertFalse(flat.truncated)
    }

    @Test
    fun `flatten caps the child levels and reports the cut`() {
        val root = level("", directory("a"), directory("b"))
        val a = level("a", file("one.txt"))
        val b = level("b", file("two.txt"))

        val flat = WorkspaceFiles.flatten(root, listOf(a, b), cap = 1)

        assertEquals(listOf("a", "b", "one.txt"), flat.rows.map { it.name })
        assertTrue(flat.truncated)
    }

    @Test
    fun `flatten reports a root with more directories than the cap`() {
        val root = level("", directory("a"), directory("b"), directory("c"))

        val flat = WorkspaceFiles.flatten(root, emptyList(), cap = 2)

        assertTrue(flat.truncated)
        assertEquals(listOf("a", "b", "c"), flat.rows.map { it.name })
    }

    @Test
    fun `flatten reports a truncated child level`() {
        val root = level("", directory("out"))
        val out = level("out", file("a.txt"), truncated = true)

        assertTrue(WorkspaceFiles.flatten(root, listOf(out)).truncated)
    }

    @Test
    fun `flatten asks for each child row's own path`() {
        val asked = mutableListOf<String>()
        val root = level("", directory("out"))
        val out = level("out", file("report.md"))

        WorkspaceFiles.flatten(root, listOf(out)) { path ->
            asked += path
            null
        }

        assertEquals(listOf("out", "out/report.md"), asked)
    }
}
