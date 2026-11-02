#pragma once

// TerminalEmulator — the terminal's own state, with no way to draw it.
//
// libvterm is a parser and a screen grid with no PTY of its own, and this client
// never opens a process or a socket (docs/terminal-contract.md). Everything
// between those two seams lives here: the libvterm instance and its lifecycle,
// the screen state (cells, cursor, palette, scrollback, the wide-glyph
// continuation, the bold-bright rule), the transport binding, and the two
// directions of bytes.
//
// WHY IT IS A CLASS OF ITS OWN. During the dual-shell period (plan §10a) both
// the QML shell and the Widgets shell are live, and both need a terminal. One
// emulator with two renderers is the only arrangement in which they cannot
// drift: a second copy of this code would be a second terminal that behaves
// subtly differently, and the difference would be found in whichever shell is
// no longer being looked at.
//
// The split is enforced by what is ABSENT from this file: no QWidget, no
// QQuickItem, no QPainter, no QML lookup. A renderer gives this object a
// transport and a viewport in pixels, reads cells back out, and paints them
// itself (src/TerminalPaint.cpp is the one painter both renderers use).
//
// TWO DECISIONS INHERITED FROM TerminalView, unchanged because they are
// behavioural and the terminal bytes depend on them:
//
//  * The transport is bound *by contract, not by class*: the signal and method
//    names below are the ones the client contract fixes, so both transports and
//    the offscreen harness's stand-in work without this file knowing a class.
//    It is INJECTED — the QML shell's item is declared in QML, so it passes the
//    context property in; a C++ caller passes its transport to the constructor.
//
//  * Bytes go in and keystrokes come out, in arrival order, undecoded. An escape
//    sequence or a UTF-8 sequence may be split across two emissions, and
//    libvterm's parser reassembles them for free; decoding to QString first
//    would destroy exactly that property.

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QSizeF>
#include <QString>
#include <QVariantList>

#include <vector>

class QTimer;

#include <vterm.h>

class TerminalEmulator : public QObject
{
    Q_OBJECT

public:
    explicit TerminalEmulator(QObject *parent = nullptr);
    ~TerminalEmulator() override;

    // --- the transport binding -------------------------------------------------

    /// Bind an injected transport. Both transports work through the contract
    /// names, and a transport with no terminal surface at all (the ACP stdio
    /// one) leaves `supported()` false rather than throwing from inside a paint.
    /// The object is not taken ownership of.
    void setTransport(QObject *transport);
    QObject *transport() const { return m_transport; }
    QString transportName() const { return m_transportName; }

    bool supported() const { return m_supported; }
    bool attached() const { return m_attached; }
    QString terminalId() const { return m_terminalId; }
    QString hostState() const { return m_hostState; }
    int exitCode() const { return m_exitCode; }

    /// Ask the transport to create-and-attach at the current grid, or to close.
    void openTerminal();
    void closeTerminal();

    // --- the grid --------------------------------------------------------------

    int columns() const { return m_columns; }
    int rows() const { return m_rows; }

    int maxColumns() const { return m_maxColumns; }
    void setMaxColumns(int columns);
    int maxRows() const { return m_maxRows; }
    void setMaxRows(int rows);

    /// The terminal's size in pixels, supplied by whichever renderer is on
    /// screen. The grid is derived from it, so both renderers must call this
    /// with their own size — and only here is the PTY's grid decided.
    void setViewport(const QSizeF &size);
    QSizeF viewport() const { return m_viewport; }

    // --- font and cell metrics -------------------------------------------------

    /// A terminal grid is only legible in a fixed pitch, so a family that cannot
    /// supply one is refused rather than accepted. The font lives here and not
    /// in a renderer because the cell size is what turns a viewport into a grid:
    /// two renderers measuring differently would ask the host's PTY for two
    /// different grids at the same window size.
    QString fontFamily() const { return m_font.family(); }
    void setFontFamily(const QString &family);
    int fontPixelSize() const { return m_font.pixelSize(); }
    void setFontPixelSize(int pixels);
    const QFont &font() const { return m_font; }
    qreal cellWidth() const { return m_cellWidth; }
    qreal cellHeight() const { return m_cellHeight; }
    qreal ascent() const { return m_ascent; }

    // --- the palette -----------------------------------------------------------

    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &colour);
    QColor background() const { return m_background; }
    void setBackground(const QColor &colour);
    QColor cursorColor() const { return m_cursorColor; }
    void setCursorColor(const QColor &colour);
    QVariantList ansiPalette() const { return m_palette; }
    void setAnsiPalette(const QVariantList &palette);

    /// One cell colour, resolved through libvterm's palette. This is where the
    /// bold-bright rule lives: a bold foreground on the low half of the palette
    /// is drawn bright, which is the convention every terminal shares and the
    /// reason coloured `ls` output is legible on a dark background at all.
    QColor resolveColour(const VTermColor &colour, bool foreground, bool bold) const;

    // --- bytes in --------------------------------------------------------------

    /// Host bytes, exactly as they arrived. Not decoded, not split.
    void feed(const QByteArray &bytes);

    // --- bytes out -------------------------------------------------------------

    /// One keystroke, through the same path as a real key event.
    void sendKey(int key, const QString &text, Qt::KeyboardModifiers modifiers);

    /// Paste text as a paste (bracketed where the remote end asked for it).
    void paste(const QString &text);

    /// Text as typing: the same code-point mapping as a paste without the
    /// bracketed-paste markers. An input method's committed string is this, not
    /// a paste — an editor must not be told a composition was pasted.
    void sendText(const QString &text);

    // --- the mouse, in the PTY's own numbering ---------------------------------

    /// Wheel notches (positive is up). A program that asked for the mouse gets
    /// the wheel as PTY buttons; one that did not gets the viewport's history.
    void wheel(int steps, Qt::KeyboardModifiers modifiers);

    /// Forwarded only while the running program is reporting the mouse. The
    /// caller checks mouseReporting() first so it can claim the click for text
    /// selection when the program has not asked for it.
    void mousePress(Qt::MouseButton button, int row, int column, Qt::KeyboardModifiers modifiers);
    void mouseRelease(Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void mouseMove(int row, int column, Qt::KeyboardModifiers modifiers);

    bool mouseReporting() const { return m_mouseMode != VTERM_PROP_MOUSE_NONE; }
    int mouseMode() const { return m_mouseMode; }

    /// The button a program was told about and has not heard the end of. A
    /// renderer that loses the grab (a QWidget's UngrabMouse, a Quick item's
    /// mouseUngrabEvent) has no release event to forward, and a PTY left
    /// believing a button is down turns every later move into a drag.
    void releaseHeldButton();
    bool buttonHeld() const { return m_heldPtyButton >= 0; }

    // --- scrollback ------------------------------------------------------------

    int scrollbackLines() const { return int(m_scrollback.size()); }
    int scrollOffset() const { return m_scrollOffset; }
    bool atBottom() const { return m_scrollOffset == 0; }

    /// Scroll the view into the history, in lines. Positive scrolls back.
    void scrollBy(int lines);
    /// Return to the live screen and stay there.
    void scrollToBottom();

    // --- reading the screen back ----------------------------------------------

    /// The grid as plain text, one line per row, trailing blank rows dropped.
    /// Read back out of libvterm rather than kept alongside it, so it is what is
    /// actually on the screen: this is how a test checks the emulator without
    /// looking at a picture.
    QString screenText() const;

    /// The live screen's cell, with libvterm's own success/failure.
    bool screenCell(int row, int column, VTermScreenCell *cell) const;

    /// The right half of a double-width glyph. libvterm marks it with all bits
    /// set rather than a documented constant; drawing it would paint the wide
    /// glyph a second time one column over, and copying it would duplicate the
    /// letter. This is a property of the cell, not of a renderer, so it is
    /// answered once, here.
    static bool isContinuation(const VTermScreenCell &cell)
    {
        return cell.chars[0] == 0xFFFFFFFFu;
    }

    /// The text of one cell, or empty for a blank cell. Shared by the painter
    /// and by selection, so a copied line is exactly what was painted.
    static QString cellText(const VTermScreenCell &cell);

    /// One cell of the buffer, where line 0 is the oldest history line and the
    /// live screen follows it. This is the coordinate space a renderer paints
    /// after it has chosen its first viewport line, and the space a selection
    /// spans. False means "nothing to draw here": the live screen refused the
    /// cell, or it is a history cell that was never written.
    bool bufferCell(int line, int column, VTermScreenCell *cell) const;

    VTermPos cursorPos() const;
    bool cursorVisible() const { return m_cursorVisible; }

    /// Viewport arithmetic, shared by the painter and by selection so that the
    /// row a click lands on and the row a glyph is painted on cannot disagree.
    int viewportColumns(qreal width) const;
    int viewportRows(qreal height) const;
    /// The buffer line the viewport's first row shows, at the current offset.
    int firstViewportLine(qreal height) const;

signals:
    /// Something on screen moved. Both renderers connect this to their own
    /// repaint request; the emulator never repaints anything itself.
    void damaged();

    void supportedChanged();
    void attachedChanged();
    void hostStateChanged();
    void gridChanged();
    void limitsChanged();
    void fontChanged();
    void paletteChanged();
    void scrollbackChanged();

private slots:
    void onTerminalOutput(const QByteArray &data);
    void onTerminalState(const QString &state, int exitCode);
    void onTerminalChanged();

    /// The grid stopped moving. Everything told to the host about the grid goes
    /// through here, so a fold animation or a window drag is one round trip
    /// rather than one per frame.
    void notifyHostOfGrid();

    /// Send the bytes the keyboard layer produced in this event-loop turn.
    void flushOutput();

private:
    /// libvterm screen callbacks. Damage is the only one that matters: the
    /// renderers repaint the whole grid, so all damage has to do is say
    /// "something moved".
    static int damageThunk(VTermRect rect, void *user);
    static int setTermPropThunk(VTermProp prop, VTermValue *value, void *user);

    /// libvterm's keyboard layer emits its bytes here. They are handed on in the
    /// same event-loop turn they were produced in — see flushOutput() for why
    /// more than one keyboard call must not become more than one write.
    static void outputThunk(const char *bytes, size_t length, void *user);

    void bindTransport();
    void connectTransportSignals();

    void writeToTransport(const QByteArray &bytes);
    void handleKey(int key, const QString &text, Qt::KeyboardModifiers modifiers);

    void recomputeMetrics();
    void applyPalette();
    void updateGrid();

    /// Invoke one of the contract's Q_INVOKABLE methods, tolerating a transport
    /// that does not implement it (the ACP one) instead of asserting.
    void callTransport(const char *method, int first = 0, int second = 0);
    bool transportHas(const char *method) const;

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    VTermState *m_state = nullptr;

    QPointer<QObject> m_transport;
    QString m_transportName;
    bool m_supported = false;
    bool m_attached = false;
    QString m_terminalId;
    QString m_hostState;
    int m_exitCode = -1;

    /// Bytes handed to us that libvterm did not consume. Reaching for this is a
    /// last resort — an escape sequence does *not* need it, because libvterm's
    /// parser keeps its state across input_write calls.
    QByteArray m_pending;

    /// A terminal was asked for before the grid had settled — the panel folds
    /// open over ~140ms, so the first frames are one or three rows tall. The
    /// request is held here and made once the grid stops moving, at the size the
    /// panel will actually draw.
    bool m_openRequested = false;
    QTimer *m_gridTimer = nullptr;

    /// Keystrokes produced since the last flush. Not a typing buffer: it lives
    /// for exactly one turn of the event loop (see flushOutput).
    QByteArray m_outbox;
    bool m_flushScheduled = false;

    /// Set by the damage callback, cleared by the repaint request that consumes
    /// it.
    bool m_damaged = true;
    bool m_cursorVisible = true;

    QFont m_font;
    QSizeF m_viewport;

    /// One history line, kept as cells rather than text so colour survives: a
    /// plain string would lose exactly the attributes a terminal exists to show.
    /// A fixed-capacity ring, because an unbounded one is a slow leak in a
    /// long-lived shell.
    struct HistoryLine {
        std::vector<VTermScreenCell> cells;
    };
    std::vector<HistoryLine> m_scrollback;
    int m_scrollOffset = 0;   ///< 0 = live screen; N = N lines back

    /// What the running program asked for. NONE means the mouse is the
    /// renderer's, which is the case until a program like `vim` or `less`
    /// enables reporting.
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;

    /// A libvterm button index that is down, or -1. Hover and ungrab events
    /// carry no button state, so DRAG reporting needs this — and so does a
    /// release that no event will ever deliver.
    int m_heldPtyButton = -1;
    static constexpr int kScrollbackLimit = 2000;

    VTermScreenCell historyCell(int line, int column) const;
    static int pushLineThunk(int cols, const VTermScreenCell *cells, void *user);
    static int popLineThunk(int cols, VTermScreenCell *cells, void *user);

    qreal m_cellWidth = 8.0;
    qreal m_cellHeight = 16.0;
    qreal m_ascent = 12.0;

    int m_columns = 0;
    int m_rows = 0;
    int m_maxColumns = 500;   ///< measured from `terminal/environment`, 2026-10-04
    int m_maxRows = 200;

    QColor m_foreground;
    QColor m_background;
    QColor m_cursorColor;
    QVariantList m_palette;
};
