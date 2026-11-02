// TerminalPanel — see TerminalPanel.h for the contract and the two invariants
// (the `shown`/`expandedHeight` height and the lazy open). Every fact about the
// terminal itself is TerminalWidget's and TerminalEmulator's; this file is the
// dock's chrome around them.

#include "TerminalPanel.h"

#include "ShellChrome.h"
#include "TerminalEmulator.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QApplication>
#include <QColor>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QVariant>

#include <functional>

namespace {

// The panel's own numbers, copied from qml/TerminalPanel.qml rather than chosen:
// the header is 35 px with the panel's 10 px side inset and a 1 px top rule; the
// body is inset 10 px on the sides and 10 px at the bottom, with a 1 px border and
// 4 px of air around the grid. The QML's fold animation is 140 ms (main.qml:4721);
// the emulator's settled-grid timer is the 120 ms that animation made necessary,
// and it is the emulator's, not this file's.
constexpr int HeaderHeight = 35;
constexpr int PanelInset = 10;
constexpr int TopRule = 1;
constexpr int BodyMargin = 10;
constexpr int GridMargin = 4;

} // namespace

// ===========================================================================
// Label — one line of text, painted with the chrome's elide rule
// ===========================================================================
//
// The QML's Text items are a label that elides (`stateLabel`) and a label that
// is clickable (`terminalScrollIndicator`). QLabel has no elide at all, so a
// QLabel would either clip mid-glyph or force the row wider than the panel; the
// one painter the header and the status strip already share is used instead.
class TerminalPanel::Label : public QWidget
{
public:
    Label(QWidget *parent, const QFont &font, const QColor &colour)
        : QWidget(parent)
        , m_font(font)
        , m_colour(colour)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        // Height comes from the font, not from a QLabel's implicit height: the
        // header is a fixed 35 px and the line sits centred in it.
        setFixedHeight(QFontMetrics(m_font).height());
    }

    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    void setColour(const QColor &colour)
    {
        if (colour == m_colour)
            return;
        m_colour = colour;
        update();
    }

    void setAlignment(Qt::Alignment alignment)
    {
        m_alignment = alignment;
        update();
    }

    /// A label with a handler is the QML's Text + MouseArea pair: the scrollback
    /// indicator is the one of those in this panel.
    void setClickHandler(std::function<void()> handler)
    {
        m_click = std::move(handler);
        setCursor(m_click ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setAttribute(Qt::WA_TransparentForMouseEvents, !m_click);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(m_font);
        return QSize(metrics.horizontalAdvance(m_text) + 2 * Theme::instance()->space1(),
                     metrics.height());
    }

    /// The state label is the greedy one in the header (`Layout.fillWidth: true`
    /// in the QML). Without this it would take its whole sentence and squeeze the
    /// indicator and the buttons out of the row.
    QSize minimumSizeHint() const override { return QSize(0, QFontMetrics(m_font).height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        Chrome::paintLine(painter, rect(), m_font, m_colour, m_text, m_alignment);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_click
            && rect().contains(event->position().toPoint())) {
            m_click();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QFont m_font;
    QColor m_colour;
    QString m_text;
    Qt::Alignment m_alignment = Qt::AlignLeft | Qt::AlignVCenter;
    std::function<void()> m_click;
};

// ===========================================================================
// Button — one push control, DshButton's recipe
// ===========================================================================
//
// The QML panel uses `DshButton` twice in its header (Close, Open/Reopen) and
// once in the empty state (Open terminal). The composer has the same recipe in
// its own private PushButton; a shared control vocabulary is the plan's later
// work item, so this is a third copy rather than a premature extraction — and a
// copy of a documented recipe is easier to compare with the QML than a new
// abstraction with three callers and one shape.
class TerminalPanel::Button : public QWidget
{
public:
    Button(const QString &text, int fixedWidth, int fixedHeight, QWidget *parent)
        : QWidget(parent)
        , m_text(text)
        , m_fixedWidth(fixedWidth)
        , m_fixedHeight(fixedHeight)
    {
        setAttribute(Qt::WA_Hover);
        // The QML buttons are MouseAreas and take no keyboard focus, the same
        // choice the header's chips and the composer's buttons make. Nothing in
        // this panel is keyboard-reachable except the terminal grid itself.
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        updateGeometry();
        update();
    }

    /// `highlighted` is DshButton's affirmative ring, drawn inside the border so
    /// a highlighted and a plain control are the same size.
    void setHighlighted(bool highlighted)
    {
        m_highlighted = highlighted;
        update();
    }

    void setHandler(std::function<void()> handler) { m_click = std::move(handler); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
        const int width = m_fixedWidth > 0
                              ? m_fixedWidth
                              : qMax(64, metrics.horizontalAdvance(m_text) + theme->space6());
        return QSize(width, m_fixedHeight);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            update();
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // DshButton's `opacity: enabled ? 1 : 0.55`: a disabled control is the
        // same object, dimmed, not a different one.
        if (!isEnabled())
            painter.setOpacity(0.55);

        const bool down = m_pressed && isEnabled();
        const bool hot = underMouse() && isEnabled();
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QLinearGradient face(body.topLeft(), body.bottomLeft());
        if (down) {
            face.setColorAt(0.0, theme->buttonPressed());
            face.setColorAt(1.0, theme->buttonPressed());
        } else if (hot) {
            face.setColorAt(0.0, theme->buttonHover());
            face.setColorAt(1.0, theme->buttonGradientTop());
        } else {
            // Lit from above: the lighter stop is the top one.
            face.setColorAt(0.0, theme->buttonGradientTop());
            face.setColorAt(1.0, theme->buttonGradientBottom());
        }

        const int radius = theme->radius();
        painter.setBrush(face);
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, radius, radius);

        // The two-tone border: Adwaita's darker bottom edge and the inset top
        // highlight, both of which the flat fill alone does not supply.
        painter.setPen(QPen(theme->buttonBorderBottom(), theme->hairline()));
        painter.drawLine(QPointF(body.left() + radius, body.bottom()),
                         QPointF(body.right() - radius, body.bottom()));
        if (!down) {
            painter.setPen(QPen(theme->insetHighlight(), theme->hairline()));
            painter.drawLine(QPointF(body.left() + radius, body.top() + 1),
                             QPointF(body.right() - radius, body.top() + 1));
        }

        // The affirmative ring sits inside the border, so a highlighted button
        // and a plain one are the same size (DshButton.qml).
        if (m_highlighted && isEnabled()) {
            const QRectF ring = body.adjusted(theme->hairline() + 1, theme->hairline() + 1,
                                              -(theme->hairline() + 1),
                                              -(theme->hairline() + 1));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            painter.drawRoundedRect(ring, qMax(0, radius - 1), qMax(0, radius - 1));
        }

        // The label: `fg` on a live control, `faint` on a disabled one, with the
        // one-pixel text shadow the gradient needs, and a one-pixel nudge down
        // while pressed.
        const QFont font = Chrome::uiFont(theme->fontUI());
        const QRectF label = body.adjusted(theme->space2(), 0, -theme->space2(), 0)
                                 .translated(0, down ? 1 : 0);
        if (isEnabled()) {
            painter.setFont(font);
            painter.setPen(theme->textShadow());
            painter.drawText(label.translated(0, -1), Qt::AlignCenter, m_text);
        }
        painter.setFont(font);
        painter.setPen(isEnabled() ? theme->fg() : theme->faint());
        painter.drawText(label, Qt::AlignCenter, m_text);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressed = true;
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool wasPressed = m_pressed;
        m_pressed = false;
        update();
        if (wasPressed && event->button() == Qt::LeftButton
            && rect().contains(event->position().toPoint()) && m_click) {
            m_click();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        update();
    }

private:
    QString m_text;
    int m_fixedWidth;
    int m_fixedHeight;
    bool m_highlighted = false;
    bool m_pressed = false;
    std::function<void()> m_click;
};

// ===========================================================================
// Body — the grid's well, and the focus ring
// ===========================================================================
//
// The QML's `body` Rectangle: terminalBg, a 1 px border, 5 px radius, the grid
// inset 4 px. Its border is the focus affordance — accent while the emulator owns
// the keyboard, `border` otherwise — because a terminal that has silently lost
// focus swallows typing with no other visible sign.
class TerminalPanel::Body : public QWidget
{
public:
    Body(TerminalWidget *terminal, QWidget *parent)
        : QWidget(parent)
        , m_terminal(terminal)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(GridMargin, GridMargin, GridMargin, GridMargin);
        layout->setSpacing(0);
        layout->addWidget(m_terminal);
    }

    /// The empty-state column is a child of the body rather than of the terminal
    /// so it covers the empty grid, exactly as the QML's overlay does. It is
    /// placed by hand because it must sit on top of the laid-out terminal, not
    /// beside it.
    void setOverlay(QWidget *overlay)
    {
        m_overlay = overlay;
        if (m_overlay != nullptr) {
            m_overlay->setGeometry(rect());
            m_overlay->raise();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const qreal radius = theme->radius();
        QPainterPath well;
        well.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter.fillPath(well, theme->terminalBg());
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_terminal->hasFocus() ? theme->accent() : theme->border(),
                            theme->hairline()));
        painter.drawPath(well);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        if (m_overlay != nullptr)
            m_overlay->setGeometry(rect());
        QWidget::resizeEvent(event);
    }

private:
    TerminalWidget *m_terminal;
    QWidget *m_overlay = nullptr;
};

// ===========================================================================
// EmptyState — "nothing to draw yet", over the empty grid
// ===========================================================================
//
// The QML column centred in the body: a sentence about the transport's state and
// an Open terminal button. It is five words over a black rectangle when there is
// no terminal, which reads as a stated condition rather than as a broken render.
class TerminalPanel::EmptyState : public QWidget
{
public:
    explicit EmptyState(QWidget *parent)
        : QWidget(parent)
    {
        Theme *theme = Theme::instance();
        auto *column = new QVBoxLayout(this);
        // The QML bounds the column to min(parent.width - 40, 360) and centres it;
        // 40 px on each side is the same intent in a layout, and the wrap handles
        // the narrow case.
        column->setContentsMargins(40, 0, 40, 0);
        column->setSpacing(10);
        column->addStretch(1);

        m_words = new QLabel(this);
        m_words->setWordWrap(true);
        m_words->setAlignment(Qt::AlignCenter);
        m_words->setFont(Chrome::uiFont(theme->fontUI()));
        QPalette wordsPalette = m_words->palette();
        wordsPalette.setColor(QPalette::WindowText, theme->dim());
        m_words->setPalette(wordsPalette);
        column->addWidget(m_words);

        m_open = new Button(QStringLiteral("Open terminal"), 0, theme->controlHeight(), this);
        m_open->setHighlighted(true);
        column->addWidget(m_open, 0, Qt::AlignHCenter);
        column->addStretch(1);
    }

    void setWords(const QString &words) { m_words->setText(words); }
    void setOpenEnabled(bool enabled) { m_open->setEnabled(enabled); }
    void setHandler(std::function<void()> handler) { m_open->setHandler(std::move(handler)); }

private:
    QLabel *m_words = nullptr;
    Button *m_open = nullptr;
};

// ===========================================================================
// the panel
// ===========================================================================

TerminalPanel::TerminalPanel(QObject *transport, QWidget *parent)
    : QWidget(parent)
    , m_transport(transport)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("terminalPanel"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // The panel fills its own rectangle in paintEvent, so Qt need not clear
    // behind it; the QML panel paints Theme.bg under the header and the body.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);

    m_terminal = new TerminalWidget(transport, this);
    // The grid's ceiling from `terminal/environment`: clamped here because the PTY
    // is resized from this side, and a grid the host will not build is worse than
    // a smaller one. The same 500x200 the QML panel states.
    m_terminal->emulator()->setMaxColumns(500);
    m_terminal->emulator()->setMaxRows(200);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(PanelInset, TopRule, PanelInset, BodyMargin);
    column->setSpacing(0);

    // ---- the header row ----------------------------------------------------
    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("terminalHeader"));
    m_header->setFixedHeight(HeaderHeight);

    auto *row = new QHBoxLayout(m_header);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *title = new Label(m_header, Chrome::uiFont(theme->fontUI(), true), theme->fg());
    title->setText(QStringLiteral("Terminal"));
    title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    row->addWidget(title);

    // The one line that says what the panel is attached to and what the host
    // thinks of it. It is greedy, as the QML's `Layout.fillWidth` is, so the
    // indicator and the buttons are never squeezed out.
    m_state = new Label(m_header, Chrome::uiFont(theme->fontData()), theme->dim());
    m_state->setObjectName(QStringLiteral("terminalStateLabel"));
    m_state->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(m_state, 1);

    // Only while actually scrolled back: a permanent indicator would be noise,
    // and its absence is the signal that the view is live (TerminalPanel.qml).
    // A label with a handler gives the QML's Text + MouseArea: clicking it
    // returns to the live screen.
    m_scroll = new Label(m_header, Chrome::uiFont(theme->fontData()), theme->accent());
    m_scroll->setObjectName(QStringLiteral("terminalScrollIndicator"));
    m_scroll->setClickHandler([this]() { m_terminal->scrollToBottom(); });
    row->addWidget(m_scroll);

    m_close = new Button(QStringLiteral("Close"), 62, 25, m_header);
    m_close->setHandler([this]() { m_terminal->closeTerminal(); });
    row->addWidget(m_close);

    m_open = new Button(QStringLiteral("Open"), 72, 25, m_header);
    m_open->setHighlighted(true);
    m_open->setHandler([this]() {
        // A terminal the host has stopped still holds our id, and asking to
        // create it again would adopt the dead shell. Dropping it first is what
        // makes this a reopen rather than a reattach (TerminalPanel.qml).
        if (m_terminal->attached())
            m_terminal->closeTerminal();
        m_terminal->openTerminal();
    });
    row->addWidget(m_open);

    column->addWidget(m_header);

    // ---- the grid, and the empty state over it ------------------------------
    m_body = new Body(m_terminal, this);
    m_body->setObjectName(QStringLiteral("terminalBody"));
    column->addWidget(m_body, 1);

    m_empty = new EmptyState(m_body);
    m_empty->setObjectName(QStringLiteral("terminalEmptyState"));
    m_empty->setHandler([this]() { m_terminal->openTerminal(); });
    m_body->setOverlay(m_empty);

    // The focus ring is this panel's; the widget reports focus changes as events.
    m_terminal->installEventFilter(this);

    QObject::connect(m_terminal, &TerminalWidget::attachedChanged, this, [this]() {
        updateChrome();
        // The QML calls forceActiveFocus() the moment the panel is unfolded; a
        // widget cannot focus a disabled child, so the only equivalent is to take
        // the focus at the moment the attach makes the grid usable.
        if (m_terminal->attached() && m_shown && m_focusOnAttach) {
            m_focusOnAttach = false;
            m_terminal->setFocus(Qt::OtherFocusReason);
        }
    });
    QObject::connect(m_terminal, &TerminalWidget::hostStateChanged, this,
                     &TerminalPanel::updateChrome);
    QObject::connect(m_terminal, &TerminalWidget::gridChanged, this,
                     &TerminalPanel::updateChrome);
    QObject::connect(m_terminal, &TerminalWidget::scrollbackChanged, this,
                     &TerminalPanel::updateChrome);

    connectTransportSignals();

    // Closed and, on a transport with no terminal, absent: the QML's
    // `implicitHeight: shown ? expandedHeight : 0` with `shown` false.
    setFixedHeight(0);
    setVisible(available());
    updateChrome();

    if (available()) {
        qInfo().nospace() << "terminal panel: " << m_terminal->emulator()->transportName()
                          << " has a terminal surface; the dock is closed until the rail's"
                          << " toggle (shown false, expandedHeight " << m_expandedHeight
                          << "px)";
    } else {
        qInfo().nospace() << "terminal panel: transport "
                          << (m_transport
                                  ? QString::fromLatin1(m_transport->metaObject()->className())
                                  : QStringLiteral("none"))
                          << " has no terminal surface; the dock stays hidden";
    }
}

TerminalPanel::~TerminalPanel() = default;

// ---- the two inputs ---------------------------------------------------------

int TerminalPanel::dockHeight() const
{
    // The QML's `implicitHeight: shown ? expandedHeight : 0`, with one addition:
    // a transport that cannot serve a terminal has no dock at all. It is a
    // number, not a measurement — main.qml:1049's footer arithmetic is the same
    // rule, and the shell adds this to the composer's height.
    return (m_shown && available()) ? m_expandedHeight : 0;
}

bool TerminalPanel::available() const
{
    // The emulator's supported() is the real capability answer: it requires both
    // `openTerminal` and `writeTerminal` and both terminal signals. This is the
    // test plan §3.3 says to use instead of the QML's typeof-openTerminal, which
    // AcpCompat's no-op satisfies and therefore lies about.
    return m_terminal != nullptr && m_terminal->supported();
}

void TerminalPanel::setShown(bool shown)
{
    // The QML comment's rule: where there is no terminal surface the panel is
    // absent rather than present and dead.
    if (!available())
        shown = false;
    if (shown == m_shown) {
        updateChrome();
        return;
    }

    m_shown = shown;
    // On the way in, remember to take the keyboard when the terminal attaches;
    // on the way out there is nothing to wait for.
    m_focusOnAttach = m_shown && !m_terminal->attached();
    setFixedHeight(dockHeight());
    updateChrome();

    emit shownChanged();
    emit dockHeightChanged();

    if (m_shown) {
        // The QML's `onShownChanged { if (shown) { openIfNeeded(); ... } }`:
        // this is the FIRST half of the lazy open. At this instant the widget is
        // still zero-height, so the emulator holds the request until its own
        // settled-grid timer fires; that is deliberate, not a missed open.
        openIfNeeded();
        focusTerminal();
    }
}

void TerminalPanel::setExpandedHeight(int height)
{
    const int bounded = qMax(0, height);
    if (bounded == m_expandedHeight)
        return;
    m_expandedHeight = bounded;
    if (m_shown)
        setFixedHeight(dockHeight());
    emit dockHeightChanged();
}

void TerminalPanel::focusTerminal()
{
    if (!m_shown || !available())
        return;
    // The QML gives the grid the keyboard when the dock unfolds. When the
    // terminal is not attached yet the grid is disabled and cannot hold focus;
    // m_focusOnAttach takes it at the attach instead.
    if (m_terminal->attached())
        m_terminal->setFocus(Qt::OtherFocusReason);
}

// ---- the lazy open ----------------------------------------------------------

void TerminalPanel::openIfNeeded()
{
    // Update first: a state change is exactly when the sentence in the empty
    // state and the Open button's enablement move.
    updateChrome();

    // The QML's four conditions, in its order: supported, shown, not already
    // open, and a session to open it in. The grid is deliberately not consulted:
    // when the dock has just been unfolded the widget is still zero-height, and
    // the emulator holds the request until its own settled-grid timer fires.
    if (!available() || !m_shown || m_terminal->attached() || !canOpen())
        return;
    m_terminal->openTerminal();
}

bool TerminalPanel::canOpen() const
{
    // TerminalPanel.qml: the transport must be Ready (state 2) and a session must
    // exist before the host will build a PTY. `sessionId` is read through the
    // contract's property, not a class: both transports publish it, and a
    // transport that does not is treated as having one (the QML's
    // `!== undefined` branch), because the session test is the stricter one.
    if (!available() || m_transport == nullptr)
        return false;
    const QVariant state = m_transport->property("state");
    if (!state.isValid() || state.toInt() != 2)
        return false;
    const QVariant session = m_transport->property("sessionId");
    return !session.isValid() || !session.toString().isEmpty();
}

bool TerminalPanel::dead() const
{
    // The host has stopped the shell but still holds the terminal: it cannot be
    // typed into, so the panel says so and offers Reopen (TerminalPanel.qml).
    const QString state = m_terminal->hostState();
    return m_terminal->attached() && !state.isEmpty()
           && state != QLatin1String("running");
}

QString TerminalPanel::statusLine() const
{
    // TerminalPanel.qml's `statusLine`, including the double spaces: the host's
    // own word is used verbatim ("running" is its vocabulary, not a translation),
    // and the id and grid are the two facts a person needs to tell one terminal
    // from another.
    const QString state = m_terminal->hostState();
    const QString grid = QString::number(m_terminal->columns()) + QChar(0x00D7)
                         + QString::number(m_terminal->rows());
    const bool open = m_terminal->attached();
    const QString id = open ? m_terminal->emulator()->terminalId().left(14) : QString();

    if (!open) {
        return state.isEmpty() ? QStringLiteral("not attached")
                               : QStringLiteral("host says: ") + state;
    }

    if (!state.isEmpty() && state != QLatin1String("running")) {
        QString line = id + QStringLiteral("  ") + grid + QStringLiteral("  host says: ")
                       + state;
        if (m_terminal->exitCode() >= 0)
            line += QStringLiteral(" (exit %1)").arg(m_terminal->exitCode());
        return line;
    }

    return id + QStringLiteral("  ") + grid
           + (state.isEmpty() ? QString() : QStringLiteral("  ") + state);
}

void TerminalPanel::connectTransportSignals()
{
    if (m_transport == nullptr)
        return;

    // By contract name, like the emulator's own binding: the two transports share
    // these signal names, and a transport without them (the ACP compat surface)
    // is reported only when it claimed to have a terminal but cannot say when it
    // becomes ready.
    const QMetaObject *meta = m_transport->metaObject();
    const struct {
        const char *signal;
        const char *slot;
    } links[] = {
        {"stateChanged()", "openIfNeeded()"},
        {"sessionChanged()", "openIfNeeded()"},
    };

    for (const auto &link : links) {
        const int signalIndex = meta->indexOfSignal(link.signal);
        const int slotIndex = metaObject()->indexOfSlot(link.slot);
        if (signalIndex < 0 || slotIndex < 0) {
            if (available()) {
                qWarning().nospace() << "terminal panel: transport "
                                     << m_transport->metaObject()->className()
                                     << " has no " << link.signal
                                     << "; the lazy open cannot be retried on it";
            }
            continue;
        }
        QMetaObject::connect(m_transport, signalIndex, this, slotIndex);
    }
}

// ---- the chrome -------------------------------------------------------------

void TerminalPanel::updateChrome()
{
    Theme *theme = Theme::instance();
    const bool open = m_terminal->attached();
    const bool isDead = dead();
    const bool can = canOpen();

    // A zero-height panel still lays out (and can focus) its children unless they
    // are hidden, and the QML's `clip: true` only stops the paint. Hiding them is
    // the widget equivalent, and it keeps the closed dock out of the tab order.
    const bool visible = m_shown && available();
    m_header->setVisible(visible);
    m_body->setVisible(visible);

    // `enabled: panel.open` in the QML: typing cannot reach a terminal that is
    // not attached, and Tab cannot land in it.
    m_terminal->setEnabled(open);
    m_terminal->setFocusPolicy(open ? Qt::StrongFocus : Qt::NoFocus);

    m_state->setText(statusLine());
    m_state->setColour(isDead ? theme->danger() : theme->dim());

    m_close->setVisible(open);
    m_open->setVisible(!open || isDead);
    m_open->setText(isDead ? QStringLiteral("Reopen") : QStringLiteral("Open"));
    m_open->setEnabled(can);

    // The empty state covers the empty grid rather than leaving a black
    // rectangle, which reads as a broken render.
    m_empty->setVisible(!open);
    m_empty->setOpenEnabled(can);
    if (!can) {
        m_empty->setWords(QStringLiteral("waiting for a session to open a terminal in"));
    } else if (!open && (m_terminal->hostState() == QLatin1String("exited")
                         || m_terminal->hostState() == QLatin1String("closed"))) {
        QString words = QStringLiteral("the terminal is ") + m_terminal->hostState();
        if (m_terminal->exitCode() >= 0)
            words += QStringLiteral(" (exit code %1)").arg(m_terminal->exitCode());
        m_empty->setWords(words);
    } else {
        m_empty->setWords(QStringLiteral("no terminal open"));
    }

    // Scrolling back is transient UI state: the indicator is present only while
    // scrolled, and its text is the same position/total pair the QML prints.
    const int lines = m_terminal->scrollbackLines();
    const int offset = m_terminal->scrollOffset();
    m_scroll->setVisible(offset > 0);
    m_scroll->setText(QString(QChar(0x2191)) + QLatin1Char(' ')
                      + QString::number(lines - offset) + QStringLiteral(" / ")
                      + QString::number(lines));
}

void TerminalPanel::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.fillRect(rect(), theme->bg());
    // The panel's own top rule, a hairline above the header (TerminalPanel.qml's
    // first Rectangle child). The footer's top edge is a different seam.
    painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->border());
}

bool TerminalPanel::eventFilter(QObject *watched, QEvent *event)
{
    // The body's accent border follows the grid's focus, and QWidget has no
    // focus-changed signal; the event is the only notification there is.
    if (watched == m_terminal
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        m_body->update();
    }
    return QWidget::eventFilter(watched, event);
}
