package uk.xa0.ward.term

data class TerminalSelection(
    val anchorRow: Int,
    val anchorCol: Int,
    val focusRow: Int,
    val focusCol: Int,
) {
    private val backwards: Boolean
        get() = focusRow < anchorRow || (focusRow == anchorRow && focusCol < anchorCol)

    val startRow: Int get() = if (backwards) focusRow else anchorRow
    val startCol: Int get() = if (backwards) focusCol else anchorCol
    val endRow: Int get() = if (backwards) anchorRow else focusRow
    val endCol: Int get() = if (backwards) anchorCol else focusCol
    val isEmpty: Boolean get() = anchorRow == focusRow && anchorCol == focusCol
}

fun TerminalEmulator.selectedText(selection: TerminalSelection): String {
    if (selection.endRow < 0 || selection.startRow > rows - 1) return ""
    val firstRow = selection.startRow.coerceAtLeast(0)
    val lastRow = selection.endRow.coerceAtMost(rows - 1)
    val text = StringBuilder()
    for (rowIndex in firstRow..lastRow) {
        if (rowIndex > firstRow) text.append('\n')
        val cells = row(rowIndex).cells
        val firstCol = (if (rowIndex == firstRow) selection.startCol else 0)
            .coerceIn(0, columns - 1)
        val lastCol = (if (rowIndex == lastRow) selection.endCol else columns - 1)
            .coerceIn(0, columns - 1)
        val line = StringBuilder()
        for (col in firstCol..lastCol) {
            val cell = cells[col]
            if (cell.width == 0) continue
            line.append(cell.text())
        }
        text.append(line.toString().trimEnd(' '))
    }
    return text.toString()
}
