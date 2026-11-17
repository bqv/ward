package uk.xa0.ward.model

enum class WorkspaceFileKind {
    FILE,
    DIRECTORY,
    OTHER,
}

data class WorkspaceFileEntry(
    val name: String,
    val kind: WorkspaceFileKind,
    val size: Long? = null,
) {
    val hidden: Boolean get() = name.startsWith(".")
}

data class WorkspaceRow(
    val name: String,
    val path: String,
    val kind: WorkspaceFileKind,
    val size: Long?,
    val mtimeMillis: Long?,
    val depth: Int,
) {
    val directory: Boolean get() = kind == WorkspaceFileKind.DIRECTORY
    val openable: Boolean get() = kind != WorkspaceFileKind.OTHER
    val hidden: Boolean get() = name.startsWith(".")
}

data class WorkspaceLevel(
    val path: String,
    val entries: List<WorkspaceFileEntry>,
    val truncated: Boolean,
)

data class WorkspaceFlat(
    val rows: List<WorkspaceRow>,
    val truncated: Boolean,
)

sealed interface WorkspaceFilesState {
    data object Idle : WorkspaceFilesState
    data class Loading(val path: String) : WorkspaceFilesState
    data class Ready(
        val currentPath: String,
        val rows: List<WorkspaceRow>,
        val truncated: Boolean,
    ) : WorkspaceFilesState
    data class Failed(val path: String, val message: String) : WorkspaceFilesState
}

object WorkspaceFiles {

    const val MAX_SUBDIRECTORIES: Int = 64

    fun childPath(parent: String, name: String): String =
        if (parent.isEmpty()) name else "$parent/$name"

    fun directories(level: WorkspaceLevel, cap: Int = MAX_SUBDIRECTORIES): List<String> =
        level.entries
            .asSequence()
            .filter { it.kind == WorkspaceFileKind.DIRECTORY }
            .take(cap.coerceAtLeast(0))
            .map { childPath(level.path, it.name) }
            .toList()

    fun rowsOf(
        level: WorkspaceLevel,
        mtimeOf: (String) -> Long? = { null },
    ): List<WorkspaceRow> = level.entries.map { entry ->
        val path = childPath(level.path, entry.name)
        WorkspaceRow(
            name = entry.name,
            path = path,
            kind = entry.kind,
            size = entry.size,
            mtimeMillis = mtimeOf(path),
            depth = 0,
        )
    }

    fun flatten(
        root: WorkspaceLevel,
        children: List<WorkspaceLevel>,
        cap: Int = MAX_SUBDIRECTORIES,
        mtimeOf: (String) -> Long? = { null },
    ): WorkspaceFlat {
        val limited = children.take(cap.coerceAtLeast(0))
        val rows = ArrayList<WorkspaceRow>()
        rows += rowsOf(root, mtimeOf)
        for (child in limited) {
            rows += rowsOf(child, mtimeOf).map { it.copy(depth = 1) }
        }
        val directoryCount = root.entries.count { it.kind == WorkspaceFileKind.DIRECTORY }
        return WorkspaceFlat(
            rows = rows,
            truncated = root.truncated ||
                children.size > limited.size ||
                directoryCount > limited.size ||
                limited.any { it.truncated },
        )
    }
}
