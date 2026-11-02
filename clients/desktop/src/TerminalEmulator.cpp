// TerminalEmulator — see TerminalEmulator.h for why it is its own class and
// what it deliberately cannot do.
//
// The emulator is deliberately small: all the terminal grammar lives in
// libvterm, and everything here is the two seams either side of it — host bytes
// in, keystroke bytes out — plus the state a renderer reads.

#include "TerminalEmulator.h"

#include "Theme.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

#include <cmath>
#include <vector>

namespace {

/// The right half of a double-width glyph. libvterm marks it with all bits set
/// rather than a documented constant; drawing it would paint the wide glyph a
/// second time one column over.
constexpr uint32_t kContinuation = 0xFFFFFFFFu;

QColor colourFromVariant(const QVariant &value)
{
    // A palette handed over from QML arrives as a JS array of strings, not as
    // QColor values, so a bare value<QColor>() would silently yield black.
    if (value.canConvert<QColor>())
        return value.value<QColor>();
    return QColor(value.toString());
}

VTermModifier vtermModifiers(Qt::KeyboardModifiers modifiers)
{
    int bits = VTERM_MOD_NONE;
    if (modifiers & Qt::ShiftModifier)
        bits |= VTERM_MOD_SHIFT;
    if (modifiers & Qt::AltModifier)
        bits |= VTERM_MOD_ALT;
    if (modifiers & Qt::ControlModifier)
        bits |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(bits);
}

/// The control code a Ctrl chord stands for, or -1 when the chord has no classic
/// encoding.
///
/// This is done here rather than by handing the letter to libvterm with
/// VTERM_MOD_CTRL because libvterm only has a legacy mapping for a small set:
/// Ctrl+A..Z, space, `\`, `]`, `^` and `_` encode classically, while Ctrl+[,
/// Ctrl+?, Ctrl+digits and Ctrl+punctuation come back as kitty-protocol CSI-u
/// (`ESC [ 9 1 ; 5 u` for Ctrl+[, measured). A shell that never negotiated that
/// protocol prints it as garbage, so the eight-line table below is used instead.
int controlCodeFor(uint32_t codePoint)
{
    if (codePoint >= 'a' && codePoint <= 'z')
        return int(codePoint - 'a') + 1;
    if (codePoint >= 'A' && codePoint <= 'Z')
        return int(codePoint - 'A') + 1;
    if (codePoint == ' ')
        return 0x00;
    if (codePoint == '?')
        return 0x7f;
    if (codePoint >= '@' && codePoint <= '_')
        return int(codePoint - '@');    // @, [, \, ], ^, _
    return -1;
}

/// The character a chord is *about*, recovered from the Qt key. Needed because a
/// Ctrl chord reports the control character as its text (Ctrl+C arrives as
/// 0x03), which is not a letter that can be mapped or displayed.
uint32_t baseCodePoint(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return uint32_t('a' + (key - Qt::Key_A));
    switch (key) {
    case Qt::Key_Space:        return ' ';
    case Qt::Key_BracketLeft:  return '[';
    case Qt::Key_BracketRight: return ']';
    case Qt::Key_Backslash:    return '\\';
    case Qt::Key_AsciiCircum:  return '^';
    case Qt::Key_Underscore:   return '_';
    case Qt::Key_At:           return '@';
    case Qt::Key_Question:     return '?';
    default:                   return 0;
    }
}

/// libvterm's own button numbering, which is undocumented and NOT the xterm wire
/// numbering. Measured against 0.3.3:
///
///   libvterm  1 2 3  =>  wire  ESC[<0M  ESC[<1M  ESC[<2M   (left, middle, right)
///   libvterm  4 5    =>  wire  ESC[<64M ESC[<65M          (wheel up, down)
///
/// So the wire value is the libvterm index minus one, and feeding it wire numbers
/// directly is an off-by-one that swaps the buttons: left emitted nothing at all
/// and right arrived as middle. The wheel must be 4/5, not 64/65, or it is silent.
int ptyButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:   return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton:  return 3;
    default:               return -1;
    }
}

/// Wheel up and wheel down, in the same numbering.
constexpr int kPtyWheelUp = 4;
constexpr int kPtyWheelDown = 5;

VTermModifier ptyModifiers(Qt::KeyboardModifiers modifiers)
{
    int value = VTERM_MOD_NONE;
    if (modifiers & Qt::ShiftModifier)
        value |= VTERM_MOD_SHIFT;
    if (modifiers & Qt::AltModifier)
        value |= VTERM_MOD_ALT;
    if (modifiers & Qt::ControlModifier)
        value |= VTERM_MOD_CTRL;
    return VTermModifier(value);
}

}   // namespace

TerminalEmulator::TerminalEmulator(QObject *parent)
    : QObject(parent)
{
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPixelSize(12);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    // The defaults are the application palette, not a second table of colours:
    // src/Theme.h is the single source, and a QML panel or a Widgets panel that
    // sets nothing still gets the product's terminal rather than a private one.
    // The QML panel passes the same tokens explicitly (qml/TerminalPanel.qml).
    const Theme *theme = Theme::instance();
    m_foreground = theme->terminalFg();
    m_background = theme->terminalBg();
    m_cursorColor = theme->terminalCursor();
    m_palette = theme->ansi();

    // libvterm needs a size at construction; the real grid is derived from the
    // viewport the first time a renderer is laid out.
    m_vt = vterm_new(24, 80);
    vterm_set_utf8(m_vt, 1);

    // Every byte the keyboard layer produces goes straight to the host from here.
    vterm_output_set_callback(m_vt, &TerminalEmulator::outputThunk, this);

    m_screen = vterm_obtain_screen(m_vt);
    m_state = vterm_obtain_state(m_vt);

    static VTermScreenCallbacks callbacks = [] {
        VTermScreenCallbacks c = {};
        c.damage = &TerminalEmulator::damageThunk;
        c.settermprop = &TerminalEmulator::setTermPropThunk;
        // Scrollback is captured here or not at all: libvterm hands each line
        // over exactly once, as it leaves the top of the screen.
        c.sb_pushline = &TerminalEmulator::pushLineThunk;
        c.sb_popline = &TerminalEmulator::popLineThunk;
        return c;
    }();
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    // Whole rows: the renderers repaint the full grid, so cell-level damage
    // bookkeeping would only cost libvterm more work per byte.
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_ROW);
    // Alternate screen, or `less` and `vim` would paint over the scrollback.
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_reset(m_screen, 1);

    recomputeMetrics();
    applyPalette();

    // One round trip per settled grid, not one per frame: the panel folds open
    // with an animation, so the viewport is resized on every frame of it, and a
    // shell does not want six resize requests for one unfold.
    m_gridTimer = new QTimer(this);
    m_gridTimer->setSingleShot(true);
    m_gridTimer->setInterval(120);
    connect(m_gridTimer, &QTimer::timeout, this, &TerminalEmulator::notifyHostOfGrid);
}

TerminalEmulator::~TerminalEmulator()
{
    if (m_vt)
        vterm_free(m_vt);
}

// --- transport binding --------------------------------------------------------

void TerminalEmulator::setTransport(QObject *transport)
{
    if (!transport) {
        qWarning("terminal: no transport; the terminal cannot be bound");
        return;
    }

    m_transport = transport;
    bindTransport();
}

void TerminalEmulator::bindTransport()
{
    m_transportName = QString::fromLatin1(m_transport->metaObject()->className());

    // A transport without the terminal surface (the ACP stdio client) is not an
    // error: the panel simply does not appear, which is what keeps that window
    // exactly as it was before this file existed.
    const bool hasOpen = transportHas("openTerminal");
    const bool hasWrite = transportHas("writeTerminal");
    m_supported = hasOpen && hasWrite;

    if (m_supported) {
        connectTransportSignals();
        m_attached = m_transport->property("terminalOpen").toBool();
        m_terminalId = m_transport->property("terminalId").toString();
    } else {
        // Not an error, and not four lines of noise either: a transport with no
        // terminal simply has no panel.
        qInfo("terminal: %s has no terminal surface; the panel stays hidden",
              qPrintable(m_transportName));
    }
    emit supportedChanged();
    emit attachedChanged();
    emit damaged();
}

bool TerminalEmulator::transportHas(const char *method) const
{
    if (!m_transport)
        return false;
    // Both contract methods are matched on the name alone: that is enough to
    // tell "this transport has a terminal" from "this one does not", without
    // depending on the class.
    const QMetaObject *meta = m_transport->metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod candidate = meta->method(i);
        if (qstrcmp(candidate.name().constData(), method) == 0)
            return true;
    }
    return false;
}

void TerminalEmulator::connectTransportSignals()
{
    const QMetaObject *meta = m_transport->metaObject();
    const QMetaObject *self = this->metaObject();

    const struct {
        const char *signal;
        const char *slot;
    } links[] = {
        {"terminalOutput(QByteArray)", "onTerminalOutput(QByteArray)"},
        {"terminalState(QString,int)", "onTerminalState(QString,int)"},
        {"terminalChanged()", "onTerminalChanged()"},
        {"terminalSizeChanged()", "onTerminalChanged()"},
    };

    for (const auto &link : links) {
        const int signalIndex = meta->indexOfSignal(link.signal);
        const int slotIndex = self->indexOfSlot(link.slot);
        if (signalIndex < 0 || slotIndex < 0) {
            // The transport has the terminal calls but not the whole signal set:
            // the terminal stays inert rather than taking the window down, but it
            // is worth saying, because half a surface is a bug somewhere.
            qWarning("terminal: %s has no %s", qPrintable(m_transportName), link.signal);
            continue;
        }
        QMetaObject::connect(m_transport, signalIndex, this, slotIndex);
    }
}

void TerminalEmulator::callTransport(const char *method, int first, int second)
{
    if (!m_transport || !m_supported)
        return;
    if (!QMetaObject::invokeMethod(m_transport, method, Q_ARG(int, first), Q_ARG(int, second)))
        qWarning("terminal: %s could not be called on %s", method, qPrintable(m_transportName));
}

void TerminalEmulator::openTerminal()
{
    if (!m_transport || m_openRequested)
        return;

    // A settled grid and no pending change means the panel is already at the size
    // it will draw, so the host can be given it straight away. Otherwise the ask is
    // held until the geometry stops moving — opening at a transient three-row size
    // would build the PTY there and then resize it twice more.
    if (m_columns > 0 && m_rows > 0 && !m_gridTimer->isActive()) {
        callTransport("openTerminal", m_columns, m_rows);
        return;
    }

    m_openRequested = true;
    if (m_columns > 0 && m_rows > 0)
        m_gridTimer->start();
}

void TerminalEmulator::closeTerminal()
{
    // A close cancels a pending open: the two are opposite intentions and the
    // later one wins.
    m_openRequested = false;
    if (m_gridTimer)
        m_gridTimer->stop();
    if (m_transport && QMetaObject::invokeMethod(m_transport, "closeTerminal"))
        return;
    if (m_transport)
        qWarning("terminal: closeTerminal could not be called on %s",
                 qPrintable(m_transportName));
}

void TerminalEmulator::onTerminalChanged()
{
    if (!m_transport)
        return;

    const bool attached = m_transport->property("terminalOpen").toBool();
    const QString id = m_transport->property("terminalId").toString();
    if (attached == m_attached && id == m_terminalId)
        return;

    m_attached = attached;
    m_terminalId = id;
    if (!m_attached) {
        m_hostState.clear();
        m_exitCode = -1;
        emit hostStateChanged();
    }
    emit attachedChanged();
    emit damaged();
}

void TerminalEmulator::onTerminalState(const QString &state, int exitCode)
{
    m_hostState = state;
    m_exitCode = exitCode;
    emit hostStateChanged();
    // Deliberately no repaint request here. The QML item has never repainted on
    // a state signal alone, and an extra repaint would take the cursor off an
    // exited terminal one frame earlier than the QML shell does today — a visible
    // difference between the two shells while both are live. The next byte, or
    // the attach change, still repaints.
}

// --- bytes in -----------------------------------------------------------------

void TerminalEmulator::onTerminalOutput(const QByteArray &data)
{
    feed(data);
}

void TerminalEmulator::feed(const QByteArray &bytes)
{
    if (!m_vt || bytes.isEmpty())
        return;

    // Cleared *before* the write: libvterm 0.3.3 delivers damage during
    // vterm_input_write (measured), so clearing afterwards would throw the flag
    // away and the grid would never repaint.
    m_damaged = false;

    const QByteArray data = m_pending + bytes;
    m_pending.clear();

    const size_t used = vterm_input_write(m_vt, data.constData(), size_t(data.size()));
    if (used < size_t(data.size())) {
        // Never observed, but a short accept must not silently drop the tail.
        m_pending = data.mid(int(used));
        qWarning("terminal: libvterm consumed %zu of %d bytes", used, int(data.size()));
    }

    // A no-op when the damage already went out during the write; kept so the code
    // is correct whichever of the two libvterm chooses.
    vterm_screen_flush_damage(m_screen);

    if (m_damaged)
        emit damaged();
}

// --- bytes out ----------------------------------------------------------------

void TerminalEmulator::handleKey(int key, const QString &text, Qt::KeyboardModifiers modifiers)
{
    if (!m_vt)
        return;

    const VTermModifier mod = vtermModifiers(modifiers);

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // Modifiers are dropped on purpose. libvterm encodes Shift+Enter and
        // Ctrl+Enter as kitty CSI-u (ESC [ 1 3 ; 2 u, measured), which a shell
        // cannot read; a terminal without that protocol has exactly one Enter.
        vterm_keyboard_key(m_vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        return;
    case Qt::Key_Tab:
        // Shift+Tab is the classic ESC [ Z and must survive; Ctrl+Tab has no
        // legacy encoding, so it is sent as a plain Tab rather than as CSI-u.
        vterm_keyboard_key(m_vt, VTERM_KEY_TAB,
                           (modifiers & Qt::ShiftModifier) ? VTERM_MOD_SHIFT : VTERM_MOD_NONE);
        return;
    case Qt::Key_Backtab:
        vterm_keyboard_key(m_vt, VTERM_KEY_TAB, VTERM_MOD_SHIFT);
        return;
    case Qt::Key_Backspace:
        // Alt+Backspace is ESC DEL and is worth keeping; Ctrl+Backspace is not
        // legacy-encodable and would come back as CSI-u.
        vterm_keyboard_key(m_vt, VTERM_KEY_BACKSPACE,
                           (modifiers & Qt::AltModifier) ? VTERM_MOD_ALT : VTERM_MOD_NONE);
        return;
    case Qt::Key_Escape:     vterm_keyboard_key(m_vt, VTERM_KEY_ESCAPE, VTERM_MOD_NONE); return;
    case Qt::Key_Up:         vterm_keyboard_key(m_vt, VTERM_KEY_UP, mod); return;
    case Qt::Key_Down:       vterm_keyboard_key(m_vt, VTERM_KEY_DOWN, mod); return;
    case Qt::Key_Left:       vterm_keyboard_key(m_vt, VTERM_KEY_LEFT, mod); return;
    case Qt::Key_Right:      vterm_keyboard_key(m_vt, VTERM_KEY_RIGHT, mod); return;
    case Qt::Key_Home:       vterm_keyboard_key(m_vt, VTERM_KEY_HOME, mod); return;
    case Qt::Key_End:        vterm_keyboard_key(m_vt, VTERM_KEY_END, mod); return;
    case Qt::Key_PageUp:     vterm_keyboard_key(m_vt, VTERM_KEY_PAGEUP, mod); return;
    case Qt::Key_PageDown:   vterm_keyboard_key(m_vt, VTERM_KEY_PAGEDOWN, mod); return;
    case Qt::Key_Insert:     vterm_keyboard_key(m_vt, VTERM_KEY_INS, mod); return;
    case Qt::Key_Delete:     vterm_keyboard_key(m_vt, VTERM_KEY_DEL, mod); return;
    default:
        break;
    }

    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        const int number = key - Qt::Key_F1 + 1;
        vterm_keyboard_key(m_vt, static_cast<VTermKey>(VTERM_KEY_FUNCTION(number)), mod);
        return;
    }

    // Ctrl+Shift+V pastes here, the way every terminal does it.
    if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier)
        && (key == Qt::Key_V)) {
        paste(QGuiApplication::clipboard()->text());
        return;
    }

    const bool control = modifiers & Qt::ControlModifier;
    // AltGr reports itself as Ctrl+Alt on some platforms; there the text is a real
    // character and must be typed as one, not folded into a control code.
    const bool altGr = modifiers & Qt::GroupSwitchModifier;

    if (control && !altGr) {
        // Alt/Ctrl+letter: the letter is recovered from the key because the text
        // is the control character itself.
        const uint32_t base = baseCodePoint(key);
        if (base != 0) {
            const int code = controlCodeFor(base);
            vterm_keyboard_unichar(m_vt, code >= 0 ? uint32_t(code) : base,
                                   code >= 0 ? VTERM_MOD_NONE
                                             : static_cast<VTermModifier>(mod & ~VTERM_MOD_CTRL));
            return;
        }
    }

    bool sent = false;
    const QList<uint> codePoints = text.toUcs4();
    for (uint codePoint : codePoints) {
        // A control character in the text is a chord already handled above, or one
        // with no meaning here; forwarding it would emit kitty CSI-u.
        if (codePoint < 0x20 || codePoint == 0x7f)
            continue;
        vterm_keyboard_unichar(m_vt, codePoint, mod);
        sent = true;
    }
    if (!sent)
        return;
}

void TerminalEmulator::sendKey(int key, const QString &text, Qt::KeyboardModifiers modifiers)
{
    handleKey(key, text, modifiers);
}

void TerminalEmulator::sendText(const QString &text)
{
    if (!m_vt || text.isEmpty())
        return;

    // The same mapping a paste uses, without the bracketed-paste markers: an
    // input method's committed string is typing, not a paste, and a program that
    // treats the two differently must be able to tell them apart.
    const QList<uint> codePoints = text.toUcs4();
    for (uint codePoint : codePoints) {
        if (codePoint == '\n' || codePoint == '\r')
            vterm_keyboard_key(m_vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        else if (codePoint != '\t')
            vterm_keyboard_unichar(m_vt, codePoint, VTERM_MOD_NONE);
        else
            vterm_keyboard_key(m_vt, VTERM_KEY_TAB, VTERM_MOD_NONE);
    }
}

void TerminalEmulator::paste(const QString &text)
{
    if (!m_vt || text.isEmpty())
        return;

    // Bracketed paste when the remote end asked for it: an editor that knows the
    // difference will not re-indent what was pasted.
    vterm_keyboard_start_paste(m_vt);
    sendText(text);
    vterm_keyboard_end_paste(m_vt);

    // The keyboard layer hands its bytes on through the same path as a keystroke.
    // The whole paste is produced in one turn of the event loop and therefore
    // leaves as one write, which is what keeps a pasted line in order (see
    // flushOutput).
}

// --- the mouse ----------------------------------------------------------------

void TerminalEmulator::wheel(int steps, Qt::KeyboardModifiers modifiers)
{
    // A program that asked for the mouse gets the wheel as buttons 64/65, which is
    // how a pager or editor scrolls itself. Only when it has not asked does the
    // wheel mean our history — otherwise `less` would fight the panel.
    if (m_mouseMode != VTERM_PROP_MOUSE_NONE && m_state && steps != 0) {
        const int button = steps > 0 ? kPtyWheelUp : kPtyWheelDown;
        vterm_mouse_button(m_vt, button, true, ptyModifiers(modifiers));
        vterm_mouse_button(m_vt, button, false, ptyModifiers(modifiers));
        return;
    }

    // Three lines a notch, which is what every terminal does.
    if (steps != 0)
        scrollBy(steps * 3);
}

void TerminalEmulator::mousePress(Qt::MouseButton which, int row, int column,
                                  Qt::KeyboardModifiers modifiers)
{
    if (m_mouseMode == VTERM_PROP_MOUSE_NONE || !m_state)
        return;

    // A click in a full-screen program is also a request to be at the live
    // screen: reporting a position in history would aim the program's cursor
    // at the wrong cell.
    scrollToBottom();
    const int button = ptyButton(which);
    if (button < 0)
        return;
    m_heldPtyButton = button;
    vterm_mouse_move(m_vt, row, column, ptyModifiers(modifiers));
    vterm_mouse_button(m_vt, button, true, ptyModifiers(modifiers));
}

void TerminalEmulator::mouseRelease(Qt::MouseButton which, Qt::KeyboardModifiers modifiers)
{
    if (m_mouseMode == VTERM_PROP_MOUSE_NONE || !m_state) {
        m_heldPtyButton = -1;
        return;
    }
    const int button = ptyButton(which);
    if (button >= 0)
        vterm_mouse_button(m_vt, button, false, ptyModifiers(modifiers));
    m_heldPtyButton = -1;
}

void TerminalEmulator::mouseMove(int row, int column, Qt::KeyboardModifiers modifiers)
{
    // CLICK reports presses only, DRAG reports while a button is held, and MOVE
    // reports every movement — so the mode decides whether a bare move is news.
    //
    // The move event carries no button state, which is why the press and release
    // handlers maintain m_heldPtyButton: without it DRAG and MOVE are
    // indistinguishable and a drag in a program would arrive as a bare move.
    const bool wanted = m_mouseMode == VTERM_PROP_MOUSE_MOVE
        || (m_mouseMode == VTERM_PROP_MOUSE_DRAG && m_heldPtyButton >= 0);
    if (wanted && m_state)
        vterm_mouse_move(m_vt, row, column, ptyModifiers(modifiers));
}

void TerminalEmulator::releaseHeldButton()
{
    if (m_heldPtyButton < 0)
        return;
    if (m_state)
        vterm_mouse_button(m_vt, m_heldPtyButton, false, VTERM_MOD_NONE);
    m_heldPtyButton = -1;
}

// --- keyboard output ----------------------------------------------------------

void TerminalEmulator::outputThunk(const char *bytes, size_t length, void *user)
{
    auto *self = static_cast<TerminalEmulator *>(user);
    self->m_outbox.append(bytes, int(length));

    // Flushed at the end of this event-loop turn, not later: a write is only
    // accepted while the attachment is live and being serviced, and a byte parked
    // for a later moment lands in exactly the window where the host refuses it
    // (docs/terminal-contract.md, "the trap that cost real time").
    if (!self->m_flushScheduled) {
        self->m_flushScheduled = true;
        QTimer::singleShot(0, self, &TerminalEmulator::flushOutput);
    }
}

void TerminalEmulator::flushOutput()
{
    m_flushScheduled = false;
    if (m_outbox.isEmpty())
        return;

    // One write per turn of the event loop, deliberately. The transport issues a
    // separate HTTP request per write and does not order concurrent requests, so
    // one keyboard call per write is only safe while the caller waits between
    // them. It does not: a paste, or a key repeat, produces a whole run of
    // keyboard calls in a single turn, and measured against the live host 22
    // strokes sent as 22 requests arrived as "echoLI VE_TRMENAI_OKL" — a shell
    // that never saw the command. Coalescing the run keeps the bytes in the order
    // the emulator produced them, which is the only order the PTY can use.
    const QByteArray bytes = m_outbox;
    m_outbox.clear();
    writeToTransport(bytes);
}

void TerminalEmulator::writeToTransport(const QByteArray &bytes)
{
    if (!m_transport || !m_supported || bytes.isEmpty())
        return;

    // Forwarded as raw bytes, not as text: the shell's idea of an encoding and
    // Qt's need not agree, and the emulator has already produced the exact
    // sequence the PTY expects.
    if (!QMetaObject::invokeMethod(m_transport, "writeTerminal", Q_ARG(QByteArray, bytes)))
        qWarning("terminal: writeTerminal could not be called on %s",
                 qPrintable(m_transportName));
}

// --- grid, metrics and palette ------------------------------------------------

void TerminalEmulator::recomputeMetrics()
{
    const QFontMetricsF metrics(m_font);
    // A monospace font is not guaranteed to be monospace for every glyph, so the
    // cell is the widest advance in a representative sample rather than the
    // advance of 'M': a cell narrower than a glyph makes it overlap its neighbour.
    static const QString sample = QStringLiteral("MW@#%&08il.,|");
    qreal widest = 1.0;
    for (const QChar &ch : sample)
        widest = qMax(widest, metrics.horizontalAdvance(ch));

    m_cellWidth = widest;
    m_cellHeight = qMax(1.0, metrics.height());
    m_ascent = metrics.ascent();

    emit fontChanged();
    updateGrid();
}

void TerminalEmulator::applyPalette()
{
    if (!m_screen || !m_state)
        return;

    VTermColor fg;
    vterm_color_rgb(&fg, uint8_t(m_foreground.red()), uint8_t(m_foreground.green()),
                    uint8_t(m_foreground.blue()));
    VTermColor bg;
    vterm_color_rgb(&bg, uint8_t(m_background.red()), uint8_t(m_background.green()),
                    uint8_t(m_background.blue()));
    vterm_screen_set_default_colors(m_screen, &fg, &bg);

    // The palette lives in libvterm rather than in a lookup table here, so
    // `vterm_screen_convert_color_to_rgb` resolves both the 16 theme colours and
    // the built-in 6x6x6 cube and greys with one call.
    for (int i = 0; i < m_palette.size() && i < 16; ++i) {
        const QColor colour = colourFromVariant(m_palette.at(i));
        if (!colour.isValid())
            continue;
        VTermColor entry;
        vterm_color_rgb(&entry, uint8_t(colour.red()), uint8_t(colour.green()),
                        uint8_t(colour.blue()));
        vterm_state_set_palette_color(m_state, i, &entry);
    }

    emit paletteChanged();
    m_damaged = false;
    vterm_screen_flush_damage(m_screen);
    emit damaged();
}

void TerminalEmulator::setViewport(const QSizeF &size)
{
    m_viewport = size;
    updateGrid();
}

void TerminalEmulator::updateGrid()
{
    if (!m_vt || m_cellWidth <= 0 || m_cellHeight <= 0)
        return;
    // Folded away (or not yet laid out): keep the grid the host already knows
    // rather than announcing a one-row terminal every time the panel animates.
    if (m_viewport.width() < m_cellWidth || m_viewport.height() < m_cellHeight)
        return;

    const int columns = qBound(1, int(m_viewport.width() / m_cellWidth), m_maxColumns);
    const int rows = qBound(1, int(m_viewport.height() / m_cellHeight), m_maxRows);
    if (columns == m_columns && rows == m_rows)
        return;

    m_columns = columns;
    m_rows = rows;

    m_damaged = false;
    vterm_set_size(m_vt, rows, columns);
    vterm_screen_flush_damage(m_screen);

    emit gridChanged();
    emit damaged();

    // The PTY must agree with the emulator or a full-screen program draws to the
    // wrong grid. The host hears about it once the grid settles — see
    // notifyHostOfGrid().
    if (m_attached || m_openRequested)
        m_gridTimer->start();
}

void TerminalEmulator::notifyHostOfGrid()
{
    if (m_columns <= 0 || m_rows <= 0)
        return;

    if (!m_attached) {
        // The open that was waiting for a settled grid. Only a request that was
        // actually made is honoured: a panel that was folded away again before the
        // grid settled must not open a terminal nobody is looking at.
        if (m_openRequested) {
            m_openRequested = false;
            callTransport("openTerminal", m_columns, m_rows);
        }
        return;
    }

    callTransport("resizeTerminal", m_columns, m_rows);
}

// --- libvterm callbacks -------------------------------------------------------

int TerminalEmulator::damageThunk(VTermRect rect, void *user)
{
    Q_UNUSED(rect)
    auto *self = static_cast<TerminalEmulator *>(user);
    self->m_damaged = true;
    // Handled: the whole grid is repainted, so there is nothing to merge.
    return 1;
}

int TerminalEmulator::setTermPropThunk(VTermProp prop, VTermValue *value, void *user)
{
    auto *self = static_cast<TerminalEmulator *>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        self->m_cursorVisible = value->boolean != 0;
        emit self->damaged();
        return 1;
    }
    if (prop == VTERM_PROP_MOUSE) {
        // A program asks to receive the mouse here (`[?1000h` and friends).
        // Until it does, clicks belong to the client — selecting text, and giving
        // the grid focus — not to whatever is running in the shell. libvterm
        // offers no getter, so this is the only place to learn it.
        self->m_mouseMode = value->number;
        return 1;
    }
    // Everything else (alt screen, title) is libvterm's own business.
    return 0;
}

// --- scrollback ---------------------------------------------------------------

int TerminalEmulator::pushLineThunk(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalEmulator *>(user);
    if (!self || cols <= 0 || !cells)
        return 0;

    HistoryLine line;
    line.cells.assign(cells, cells + cols);

    self->m_scrollback.push_back(std::move(line));
    if (int(self->m_scrollback.size()) > kScrollbackLimit)
        self->m_scrollback.erase(self->m_scrollback.begin());

    // A visible shift of the window while lines are being dropped off the end
    // would read as the view jumping, so the offset is clamped to what remains.
    if (self->m_scrollOffset > int(self->m_scrollback.size()))
        self->m_scrollOffset = int(self->m_scrollback.size());

    emit self->scrollbackChanged();
    // Claim the line: we keep our own copy, so libvterm need not retain it.
    return 1;
}

int TerminalEmulator::popLineThunk(int cols, VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalEmulator *>(user);
    // Popping is the reverse (a scroll region moving back down). We do not serve
    // history back into the grid: nothing here rewrites the screen, so claiming
    // it would only lose the line.
    Q_UNUSED(cols)
    Q_UNUSED(cells)
    Q_UNUSED(self)
    return 0;
}

VTermScreenCell TerminalEmulator::historyCell(int line, int column) const
{
    VTermScreenCell blank = {};
    if (line < 0 || line >= int(m_scrollback.size()))
        return blank;
    const HistoryLine &entry = m_scrollback[size_t(line)];
    if (column < 0 || column >= int(entry.cells.size()))
        return blank;
    return entry.cells[size_t(column)];
}

void TerminalEmulator::scrollBy(int lines)
{
    const int limit = int(m_scrollback.size());
    const int wanted = qBound(0, m_scrollOffset + lines, limit);
    if (wanted == m_scrollOffset)
        return;
    m_scrollOffset = wanted;
    emit scrollbackChanged();
    emit damaged();
}

void TerminalEmulator::scrollToBottom()
{
    if (m_scrollOffset == 0)
        return;
    m_scrollOffset = 0;
    emit scrollbackChanged();
    emit damaged();
}

// --- reading the screen back --------------------------------------------------

QString TerminalEmulator::screenText() const
{
    if (!m_screen)
        return {};

    QStringList lines;
    for (int row = 0; row < m_rows; ++row) {
        QString line;
        for (int column = 0; column < m_columns; ++column) {
            VTermScreenCell cell;
            const VTermPos pos{row, column};
            if (!vterm_screen_get_cell(m_screen, pos, &cell)) {
                line += QLatin1Char(' ');
                continue;
            }
            if (cell.chars[0] == kContinuation)
                continue;   // the wide glyph to the left already covered this column
            if (cell.chars[0] == 0) {
                line += QLatin1Char(' ');
                continue;
            }
            int length = 0;
            while (length < VTERM_MAX_CHARS_PER_CELL && cell.chars[length] != 0)
                ++length;
            char32_t codePoints[VTERM_MAX_CHARS_PER_CELL];
            for (int i = 0; i < length; ++i)
                codePoints[i] = char32_t(cell.chars[i]);
            line += QString::fromUcs4(codePoints, length);
        }
        lines += line;
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

bool TerminalEmulator::screenCell(int row, int column, VTermScreenCell *cell) const
{
    if (!m_screen || !cell || row < 0 || row >= m_rows || column < 0 || column >= m_columns)
        return false;
    const VTermPos pos{row, column};
    return vterm_screen_get_cell(m_screen, pos, cell) != 0;
}

QString TerminalEmulator::cellText(const VTermScreenCell &cell)
{
    int length = 0;
    while (length < VTERM_MAX_CHARS_PER_CELL && cell.chars[length] != 0)
        ++length;
    if (length <= 0)
        return {};
    char32_t codePoints[VTERM_MAX_CHARS_PER_CELL];
    for (int i = 0; i < length; ++i)
        codePoints[i] = char32_t(cell.chars[i]);
    return QString::fromUcs4(codePoints, length);
}

bool TerminalEmulator::bufferCell(int line, int column, VTermScreenCell *cell) const
{
    if (!cell)
        return false;

    const int historyCount = int(m_scrollback.size());
    if (line >= 0 && line < historyCount) {
        *cell = historyCell(line, column);
        // A history cell that was never written has nothing to draw, exactly as
        // before: the live screen's blanks have a width and must still paint
        // their background, while an unset history slot has neither.
        return !(cell->chars[0] == 0 && cell->width == 0);
    }
    if (line < historyCount)
        return false;
    return screenCell(line - historyCount, column, cell);
}

VTermPos TerminalEmulator::cursorPos() const
{
    VTermPos cursor = {};
    if (m_state)
        vterm_state_get_cursorpos(m_state, &cursor);
    return cursor;
}

int TerminalEmulator::viewportColumns(qreal width) const
{
    if (!m_screen || m_cellWidth <= 0)
        return 0;
    return qMin(m_columns, int(std::ceil(width / m_cellWidth)) + 1);
}

int TerminalEmulator::viewportRows(qreal height) const
{
    if (!m_screen || m_cellHeight <= 0)
        return 0;
    return qMin(m_rows, int(std::ceil(height / m_cellHeight)) + 1);
}

int TerminalEmulator::firstViewportLine(qreal height) const
{
    // History extends the screen upwards: the buffer is [history][live screen],
    // and the viewport shows the LAST `rows` lines of it, shifted up by the
    // offset.
    //
    // The first version had this backwards (it started the window at
    // historyCount - offset), which at offset 0 showed the oldest history and
    // pushed the live screen out of the bottom of the viewport — a terminal that
    // silently stops showing the present.
    const int bufferTotal = int(m_scrollback.size()) + m_rows;
    return qMax(0, bufferTotal - viewportRows(height) - m_scrollOffset);
}

// --- properties ---------------------------------------------------------------

void TerminalEmulator::setMaxColumns(int columns)
{
    columns = qMax(1, columns);
    if (columns == m_maxColumns)
        return;
    m_maxColumns = columns;
    emit limitsChanged();
    updateGrid();
}

void TerminalEmulator::setMaxRows(int rows)
{
    rows = qMax(1, rows);
    if (rows == m_maxRows)
        return;
    m_maxRows = rows;
    emit limitsChanged();
    updateGrid();
}

void TerminalEmulator::setFontFamily(const QString &family)
{
    if (family.isEmpty() || family == m_font.family())
        return;

    // A terminal grid is only legible in a fixed pitch, so a family that cannot
    // provide one is refused rather than accepted.
    // This is not hypothetical: the theme named the family "monospace", Qt
    // resolved that generic alias to a PROPORTIONAL face here, and the rendered
    // grid lost its column alignment while every metric still looked plausible.
    //
    // The requested family is used when it is genuinely fixed pitch; otherwise
    // the system's fixed font is kept, which is the choice the constructor makes.
    QFont requested(family);
    requested.setStyleHint(QFont::Monospace);
    requested.setFixedPitch(true);

    const QFontInfo info(requested);
    if (!info.fixedPitch()) {
        qWarning() << "terminal: refusing proportional family" << family
                   << "-> keeping" << m_font.family();
        return;
    }

    m_font.setFamily(info.family());
    recomputeMetrics();
    emit damaged();
}

void TerminalEmulator::setFontPixelSize(int pixels)
{
    pixels = qBound(4, pixels, 72);
    if (pixels == m_font.pixelSize())
        return;
    m_font.setPixelSize(pixels);
    recomputeMetrics();
    emit damaged();
}

void TerminalEmulator::setForeground(const QColor &colour)
{
    if (!colour.isValid() || colour == m_foreground)
        return;
    m_foreground = colour;
    applyPalette();
}

void TerminalEmulator::setBackground(const QColor &colour)
{
    if (!colour.isValid() || colour == m_background)
        return;
    m_background = colour;
    applyPalette();
}

void TerminalEmulator::setCursorColor(const QColor &colour)
{
    if (!colour.isValid() || colour == m_cursorColor)
        return;
    m_cursorColor = colour;
    emit paletteChanged();
    emit damaged();
}

void TerminalEmulator::setAnsiPalette(const QVariantList &palette)
{
    if (palette.isEmpty())
        return;
    m_palette = palette;
    applyPalette();
}

QColor TerminalEmulator::resolveColour(const VTermColor &colour, bool foreground,
                                       bool bold) const
{
    if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&colour))
        return m_foreground;
    if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&colour))
        return m_background;

    VTermColor resolved = colour;
    // A bold foreground on the low half of the palette is drawn bright: the
    // convention every terminal shares, and the reason coloured `ls` output is
    // legible on a dark background at all.
    if (foreground && bold && VTERM_COLOR_IS_INDEXED(&resolved) && resolved.indexed.idx < 8) {
        VTermColor bright;
        vterm_state_get_palette_color(m_state, resolved.indexed.idx + 8, &bright);
        vterm_screen_convert_color_to_rgb(m_screen, &bright);
        if (!VTERM_COLOR_IS_DEFAULT_FG(&bright) && !VTERM_COLOR_IS_DEFAULT_BG(&bright))
            return QColor(bright.rgb.red, bright.rgb.green, bright.rgb.blue);
    }

    vterm_screen_convert_color_to_rgb(m_screen, &resolved);
    return QColor(resolved.rgb.red, resolved.rgb.green, resolved.rgb.blue);
}
