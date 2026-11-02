#include "Composer.h"

#include "ClientFacade.h"
#include "MentionPopup.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHideEvent>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QTextBlock>
#include <QTextLayout>
#include <QDebug>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>
#include <QtMath>

#include <functional>
#include <utility>

namespace {

/// main.qml:1618's own cap on the field:
/// `Layout.preferredHeight: Math.min(120, Math.max(Theme.fieldHeight,
/// input.contentHeight + Theme.space6))`. The token table has no 120, and a field
/// taller than this stops reading as a field; past it the entry scrolls instead.
constexpr int MaxFieldHeight = 120;

/// The three control states a push button paints, kept as one value rather than
/// three booleans that can disagree.
enum class ButtonState { Rest, Hover, Down };

constexpr int ThumbSide = 44;
constexpr int FileChipHeight = 28;
constexpr int MaxShownTiles = 6;
constexpr int MaxNameWidth = 132;

QString iconDirectory()
{
    static const QString directory = []() {
        QStringList candidates;
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/assets/icons")
                   << QStringLiteral(DSH_NATIVE_DATA_SUBDIR "/icons")
                   << QCoreApplication::applicationDirPath() + QStringLiteral("/../assets/icons");
        for (const QString &candidate : candidates) {
            if (QDir(candidate).exists())
                return QDir(candidate).absolutePath();
        }
        return QString();
    }();
    return directory;
}

QPixmap iconMark(const QString &name, int side)
{
    static QHash<QString, QPixmap> cache;
    const QString key = name + QLatin1Char('/') + QString::number(side);
    auto cached = cache.constFind(key);
    if (cached != cache.cend())
        return cached.value();
    QPixmap mark;
    const QString directory = iconDirectory();
    if (!directory.isEmpty()) {
        mark = QPixmap(directory + QLatin1Char('/') + name + QStringLiteral(".svg"));
        if (!mark.isNull())
            mark = mark.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    cache.insert(key, mark);
    return mark;
}

} // namespace

// ===========================================================================
// the entry
// ===========================================================================
//
// A `QPlainTextEdit` because the field is multi-line by contract (Shift+Enter is
// a newline, and a pasted paragraph has to land as one), with the QML TextArea's
// own metrics: 8 px of padding on all four sides (main.qml:1644-1647), the UI face
// at `Theme.fontUI`, and the selection pair taken from the theme rather than the
// platform's pale blue, which measured 2.3:1 on this field (main.qml:1658-1665).
//
// The entry paints no surface of its own: the QML's background rectangle — a
// rounded well, a two-tone border and an inset accent ring on focus — is
// `Field`'s paint, just below (`DshTextField.qml` gives the recipe, and it is the
// same one the QML composer copies for its TextArea). The text and the caret are
// the base class's; this class owns the type, the padding and the keyboard.
class Composer::Entry : public QPlainTextEdit
{
public:
    explicit Entry(Composer *owner, QWidget *parent)
        : QPlainTextEdit(parent)
        , m_owner(owner)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("composerEntry"));

        // No frame, and no context menu: a right-click menu is a popup, and popups
        // are the shell's (plan §4.4). The QML's TextArea has none either.
        setFrameShape(QFrame::NoFrame);
        setContextMenuPolicy(Qt::NoContextMenu);
        setBackgroundVisible(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // AsNeeded, but a single line is taller than the field once the document
        // margin is counted, so AsNeeded showed a pair of arrows inside a
        // one-line field. The QML's ScrollView has no visible bar either: the
        // scroll is real, the ornament is not.
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setCursorWidth(theme->hairline());

        // The QML's text is UI type at `Theme.fontUI`, not the platform's 9 pt,
        // which was the only text in the window outside the token table
        // (main.qml:1630-1635).
        setFont(Chrome::uiFont(theme->fontUI()));

        // `leftPadding`/`rightPadding`/`topPadding`/`bottomPadding` are all
        // `Theme.space4` in the QML (main.qml:1644-1647), so the document margin —
        // which QPlainTextEdit uses on all four sides — is that one token.
        // The QML's padding is `space4` around the text, but a QPlainTextEdit's
        // document margin is inside its own scroll range: at 8 px the document is
        // 35 px in a 33 px viewport, so the view scrolls to the caret and a
        // single line is drawn along the field's bottom edge, clipped. The QML
        // never shows that because its ScrollView pins the line with
        // `verticalAlignment: AlignVCenter`. `space2` leaves the document shorter
        // than the viewport, which is what lets the centring below do its work.
        document()->setDocumentMargin(theme->space2());

        QPalette entryPalette = palette();
        applyColours(&entryPalette);
        setPalette(entryPalette);
        viewport()->setPalette(entryPalette);

        // `verticalAlignment: AlignVCenter` (main.qml:1654) has no QPlainTextEdit
        // counterpart, so it becomes a viewport offset recomputed as the content
        // grows; see centreText().
    }

    /// The height the QML's expression gives: the content, but never below the
    /// field token and never above the cap. Past the cap the viewport is shorter
    /// than the document and QPlainTextEdit scrolls it, which is the inner scroll
    /// the QML's ScrollView also provides.
    int preferredHeight() const
    {
        Theme *theme = Theme::instance();
        return qBound(theme->fieldHeight(), textHeight(), MaxFieldHeight);
    }

    /// The lines the layout actually broke the text into.
    ///
    /// Not `blockCount()`: Shift+Enter inserts U+2028 into the CURRENT block, so
    /// after two visible lines the document still has one block (measured on this
    /// Qt: blocks 1, `document()->lineCount()` 2). `document()->lineCount()` does
    /// answer with the visible count, but that is the document layout's own
    /// choice; asking each block's `QTextLayout` is the same number asked of the
    /// layout that painted the lines, and it also counts the lines a long
    /// paragraph wraps into.
    int textLines() const
    {
        int lines = 0;
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            const QTextLayout *layout = block.layout();
            lines += layout != nullptr ? qMax(1, layout->lineCount()) : 1;
        }
        return qMax(1, lines);
    }

    /// The text's own height, including the document margin on both edges — that
    /// is why it is compared with `fieldHeight` directly and not offset by
    /// `Theme.space6` the way QML offsets `contentHeight`.
    ///
    /// Neither `document()->size()` nor the layout's documentSize() answers in
    /// pixels before the document has a width: both reported **1** for a
    /// one-line field, so the centring believed it had 32 px of slack and pushed
    /// the line to the field's bottom edge. The line count times the font's line
    /// spacing is the number QPlainTextEdit itself scrolls by.
    int textHeight() const
    {
        return QFontMetrics(font()).lineSpacing() * textLines()
               + 2 * qRound(document()->documentMargin());
    }

    /// The QML centres a short line in the minimum field and lets the content take
    /// over once there is no slack (main.qml:1649-1654). QPlainTextEdit has no
    /// verticalAlignment, so half the slack becomes the viewport's top margin: the
    /// text, the caret and hit-testing all move with it.
    void centreText()
    {
        const int slack = qMax(0, height() - textHeight());
        setViewportMargins(0, slack / 2, 0, 0);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (m_owner->onMentionKey(event->key(), event->modifiers())) {
            event->accept();
            return;
        }

        // main.qml:1745-1756. Enter and Ctrl+Enter submit, with the mode the
        // facade resolves; Shift+Enter is a newline and is never intercepted, so
        // the field's own handling inserts it.
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (event->modifiers() & Qt::ShiftModifier) {
                QPlainTextEdit::keyPressEvent(event);
                return;
            }
            m_owner->onEnterPressed(event->modifiers());
            event->accept();
            return;
        }

        // main.qml:1757-1763: Ctrl+V also offers the clipboard to the transport as
        // an image. The key is NOT accepted — ordinary text still pastes — and the
        // transport decides whether the clipboard holds an image at all.
        if (event->key() == Qt::Key_V && (event->modifiers() & Qt::ControlModifier))
            m_owner->onPasteRequested();

        // main.qml:1764-1772: Escape stops a running turn. The shell owns Escape
        // for its own modality once that stack exists (plan §4.4); until then this
        // is the field's handler and nothing else wants the key.
        if (event->key() == Qt::Key_Escape && m_owner->onEscapePressed()) {
            event->accept();
            return;
        }

        QPlainTextEdit::keyPressEvent(event);
    }

private:
    /// The QML TextArea's colours, in QPalette terms. `disabledFill` and `faint`
    /// are the visible form of the QML's `enabled: state === 2`: the QML only
    /// stops the keypress, and a well that looks typeable but is not is the worse
    /// of the two.
    void applyColours(QPalette *entryPalette) const
    {
        Theme *theme = Theme::instance();
        entryPalette->setColor(QPalette::Base, theme->fieldFill());
        entryPalette->setColor(QPalette::Window, theme->fieldFill());
        entryPalette->setColor(QPalette::Text, theme->fg());
        entryPalette->setColor(QPalette::WindowText, theme->fg());
        entryPalette->setColor(QPalette::PlaceholderText, theme->dim());
        entryPalette->setColor(QPalette::Highlight, theme->accent());
        entryPalette->setColor(QPalette::HighlightedText, theme->accentFg());

        entryPalette->setColor(QPalette::Disabled, QPalette::Base, theme->disabledFill());
        entryPalette->setColor(QPalette::Disabled, QPalette::Text, theme->faint());
        entryPalette->setColor(QPalette::Disabled, QPalette::WindowText, theme->faint());
        entryPalette->setColor(QPalette::Disabled, QPalette::PlaceholderText, theme->faint());
    }

    Composer *m_owner;
};

// ===========================================================================
// the field: the surface the entry is drawn on
// ===========================================================================
//
// The entry paints no background (`backgroundVisible(false)`), so the well, its
// rounded border and the accent ring on focus are this widget's paint: the QML's
// background rectangle, which the composer's TextArea carries (main.qml:1669-1686)
// and `DshTextField.qml` states outright. Keeping them out of the entry also keeps
// the field's edge beneath the entry's own scrollbar, which is a child of the
// entry and would otherwise paint over a border drawn on the viewport.
class Composer::Field : public QWidget
{
public:
    explicit Field(Composer *owner, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("composerField"));
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_entry = new Entry(owner, this);
        m_entry->installEventFilter(this);
    }

    Entry *entry() const { return m_entry; }


    void adoptHeight(int height)
    {
        if (height != minimumHeight()) {
            setFixedHeight(height);
            updateGeometry();
        }
        // Inside the field's own border, not over it: `rect()` includes the
        // hairline the field paints, and an entry given the whole rectangle
        // covers it with its own background.
        fitEntry();
        m_entry->centreText();
        update();
    }

    QSize sizeHint() const override
    {
        const int field = m_entry->preferredHeight();
        return QSize(200, field);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, Theme::instance()->fieldHeight());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // The half-pixel inset keeps the 1 px pen inside the widget, the way QML's
        // `border.width: 1` draws.
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setBrush(m_entry->isEnabled() ? theme->fieldFill() : theme->disabledFill());
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, theme->radius(), theme->radius());

        // Adwaita rings a focused entry with an inset accent hairline, not a
        // thicker border, so focus costs the text no geometry (DshTextField.qml's
        // own comment; the QML composer's background copies the same pairs).
        if (m_entry->hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(theme->accent(), theme->hairline()));
            const QRectF ring = body.adjusted(1, 1, -1, -1);
            painter.drawRoundedRect(ring, theme->radius() - 1, theme->radius() - 1);
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        fitEntry();
        m_entry->centreText();
    }

    /// The entry sits inside the field's painted hairline. `rect()` includes the
    /// border, and an entry given the whole rectangle covers it with its own
    /// background.
    void fitEntry()
    {
        const int hairline = Theme::instance()->hairline();
        m_entry->setGeometry(rect().adjusted(hairline, hairline, -hairline, -hairline));
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // The ring is the field's paint, so a focus change has to repaint the field
        // and not only the entry inside it.
        if (watched == m_entry
            && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
            update();
        return QWidget::eventFilter(watched, event);
    }

private:
    Entry *m_entry = nullptr;
};

// ===========================================================================
// the notice line
// ===========================================================================
//
// The refusal's home. The QML answers an unknown command with a row in the
// transcript and a word in the status strip (main.qml:2180-2191); the Widgets
// shell has both of those, but a refusal at the point of typing is the one the
// person is looking at, so the composer says it too. It is a line of the
// composer's own band — not a dialog, not a popup, and it costs the footer one
// line only while it is up.
class Composer::Notice : public QWidget
{
public:
    explicit Notice(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("composerNotice"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setVisible(false);
    }

    void setNotice(const QString &text, bool error)
    {
        m_text = text;
        m_error = error;
        setVisible(!m_text.isEmpty());
        updateGeometry();
        update();
    }

    QString text() const { return m_text; }

    QSize sizeHint() const override
    {
        if (m_text.isEmpty())
            return QSize(0, 0);
        // The line, its own gap above the entry (`space2`, which the composer's
        // layout does not add because its spacing is zero: a hidden notice must
        // cost the layout nothing at all, and a box layout's spacing around an
        // empty item is a detail this arithmetic should not depend on).
        const QFontMetrics metrics(Chrome::uiFont(Theme::instance()->fontData()));
        return QSize(metrics.horizontalAdvance(m_text),
                     metrics.height() + Theme::instance()->space1()
                         + Theme::instance()->space2());
    }

    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_text.isEmpty())
            return;
        Theme *theme = Theme::instance();
        QPainter painter(this);
        Chrome::paintLine(painter, rect(), Chrome::uiFont(theme->fontData()),
                          m_error ? theme->danger() : theme->caption(), m_text);
    }

private:
    QString m_text;
    bool m_error = false;
};

// ===========================================================================
// the text control: the mode chip and the queue count
// ===========================================================================
//
// main.qml's `enterModeHint` is a bare `Text` with a `MouseArea` — no border, no
// fill, `faint` at rest and `fg` under the pointer (main.qml:1792-1816). It is a
// control you can click, but it never looks like a button, and that is deliberate:
// it is a hint first. The queue count uses the same shape because it is the same
// kind of thing — a word beside the field, not a button that acts on the text.
class Composer::TextButton : public QWidget
{
public:
    TextButton(const QString &objectName, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(objectName);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setText(const QString &text)
    {
        m_text = text;
        updateGeometry();
        update();
    }

    QString text() const { return m_text; }

    void setHandler(std::function<void()> handler) { m_click = std::move(handler); }

    /// Whether a click does anything. The queue count is a count until a turn is
    /// running; a chip that lifts under the pointer and then does nothing is the
    /// dead affordance this window is not supposed to have.
    void setClickable(bool clickable)
    {
        if (clickable == m_clickable)
            return;
        m_clickable = clickable;
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        return QSize(metrics.horizontalAdvance(m_text) + 2 * theme->space1(),
                     metrics.height());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        const bool lit = m_clickable && underMouse();
        Chrome::paintLine(painter, rect(), Chrome::uiFont(theme->fontData()),
                          lit ? theme->fg() : theme->faint(), m_text, Qt::AlignCenter);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_clickable
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
    bool m_clickable = true;
    std::function<void()> m_click;
};

// ===========================================================================
// the push button: Send/Stop and the attach toggle
// ===========================================================================
//
// `DshButton.qml`'s recipe, painted rather than styled: a 5 px radius, a two-tone
// border, a vertical gradient and the inset top highlight. The plan puts the
// shared control vocabulary in stage 1 as its own work item; until it exists this
// is the composer's own copy of the recipe rather than a widget other slices can
// use.
//
// One deliberate correction to the QML while porting: `DshButton` sets both
// `color` and `gradient` on its face, and Qt Quick's Rectangle gives the gradient
// precedence — so the QML's hover fill never appears. The gradient here takes its
// stops from the state instead, and the button reads as lit from above, which is
// the bevel rule in docs/ui-traditions.md and what the QML's comment intends.
class Composer::PushButton : public QWidget
{
public:
    PushButton(const QString &label, int fixedWidth, QWidget *parent)
        : QWidget(parent)
        , m_text(label)
        , m_fixedWidth(fixedWidth)
    {
        setAttribute(Qt::WA_Hover);
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

    void setHandler(std::function<void()> handler) { m_click = std::move(handler); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
        const int width = m_fixedWidth > 0
                              ? m_fixedWidth
                              : qMax(64, metrics.horizontalAdvance(m_text) + theme->space6());
        return QSize(width, theme->controlHeight());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void changeEvent(QEvent *event) override
    {
        // A disabled control dims itself, so an enablement change is a repaint
        // rather than only a state flag.
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            update();
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // DshButton's `opacity: enabled ? 1 : 0.55`: a disabled control is the same
        // object, dimmed, not a different one.
        if (!isEnabled())
            painter.setOpacity(0.55);

        const ButtonState state = !isEnabled() ? ButtonState::Rest
                                  : m_pressed ? ButtonState::Down
                                  : underMouse() ? ButtonState::Hover
                                                 : ButtonState::Rest;
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QLinearGradient face(body.topLeft(), body.bottomLeft());
        if (state == ButtonState::Down) {
            face.setColorAt(0.0, theme->buttonPressed());
            face.setColorAt(1.0, theme->buttonPressed());
        } else if (state == ButtonState::Hover) {
            face.setColorAt(0.0, theme->buttonHover());
            face.setColorAt(1.0, theme->buttonGradientTop());
        } else {
            // Lit from above: the lighter stop is the top one.
            face.setColorAt(0.0, theme->buttonGradientTop());
            face.setColorAt(1.0, theme->buttonGradientBottom());
        }

        painter.setBrush(face);
        painter.setPen(QPen(theme->buttonBorder(), theme->hairline()));
        painter.drawRoundedRect(body, theme->radius(), theme->radius());

        // `border-bottom-color` is a second, darker hairline along the bottom
        // edge, and `inset 0 1px rgba(255,255,255,0.02)` one along the top inside.
        // Both are what make the control read as milled rather than flat.
        const int radius = theme->radius();
        painter.setPen(QPen(theme->buttonBorderBottom(), theme->hairline()));
        painter.drawLine(QPointF(body.left() + radius, body.bottom()),
                         QPointF(body.right() - radius, body.bottom()));
        if (state != ButtonState::Down) {
            painter.setPen(QPen(theme->insetHighlight(), theme->hairline()));
            painter.drawLine(QPointF(body.left() + radius, body.top() + 1),
                             QPointF(body.right() - radius, body.top() + 1));
        }

        // The label: `fg` on a live control, `faint` on a disabled one, with the
        // one-pixel text shadow the gradient needs (DshButton.qml's label), and
        // the press nudging it down a pixel.
        const QFont font = Chrome::uiFont(theme->fontUI());
        const QRectF label = body.adjusted(theme->space2(), 0, -theme->space2(), 0)
                                 .translated(0, state == ButtonState::Down ? 1 : 0);
        if (isEnabled()) {
            painter.setFont(font);
            painter.setPen(theme->textShadow());
            painter.drawText(label.translated(0, -1),
                             int(Qt::AlignCenter), m_text);
        }
        painter.setPen(isEnabled() ? theme->fg() : theme->faint());
        painter.drawText(label, int(Qt::AlignCenter), m_text);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
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
        if (event->button() == Qt::LeftButton && wasPressed && isEnabled()
            && rect().contains(event->position().toPoint()) && m_click) {
            m_click();
            event->accept();
        }
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
    int m_fixedWidth = 0;
    bool m_pressed = false;
    std::function<void()> m_click;
};

class Composer::Strip : public QWidget
{
public:
    struct Tile
    {
        QString name;
        bool image = false;
        QPixmap thumb;
        int width = 0;
    };

    explicit Strip(Composer *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        setObjectName(QStringLiteral("composerAttachments"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFixedHeight(stripHeight());
        setMouseTracking(true);
    }

    static int stripHeight()
    {
        Theme *theme = Theme::instance();
        return ThumbSide + 2 * theme->space2();
    }

    int attachmentCount() const { return m_tiles.size() + m_overflow; }

    void refresh()
    {
        const QVariantList items = m_owner->m_facade->pendingAttachments();
        m_tiles.clear();
        m_overflow = 0;
        const int shown = qMin(int(items.size()), MaxShownTiles);
        for (int index = 0; index < shown; ++index)
            m_tiles.append(tileFor(items.at(index).toMap()));
        if (items.size() > shown)
            m_overflow = items.size() - shown;
        m_clearRect = QRect();
        setVisible(!items.isEmpty());
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const int gap = theme->space3();
        const int top = (height() - ThumbSide) / 2;
        const int chipTop = top + (ThumbSide - FileChipHeight) / 2;
        int x = 0;
        for (const Tile &tile : std::as_const(m_tiles)) {
            const QRect slot(x, top, tile.width, ThumbSide);
            if (tile.image)
                paintThumb(painter, slot, tile);
            else
                paintFile(painter, slot, tile);
            x = slot.right() + 1 + gap;
        }
        if (m_overflow > 0) {
            const QString label = QStringLiteral("+%1").arg(m_overflow);
            const QRect slot(x, chipTop, labelWidth(label), FileChipHeight);
            Chrome::paintChip(painter, slot, false, false);
            Chrome::paintLine(painter, slot, Chrome::uiFont(theme->fontData()), theme->dim(),
                              label, Qt::AlignCenter);
            x = slot.right() + 1 + gap;
        }
        if (!m_tiles.isEmpty()) {
            const QString label = QStringLiteral("Clear");
            const QRect slot(x, chipTop, labelWidth(label), FileChipHeight);
            Chrome::paintChip(painter, slot, m_clearHover, false);
            Chrome::paintLine(painter, slot, Chrome::uiFont(theme->fontData()), theme->dim(),
                              label, Qt::AlignCenter);
            m_clearRect = slot;
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool over = m_clearRect.contains(event->position().toPoint());
        if (over != m_clearHover) {
            m_clearHover = over;
            update();
        }
        setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (m_clearHover) {
            m_clearHover = false;
            update();
        }
        setCursor(Qt::ArrowCursor);
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_clearRect.contains(event->position().toPoint())) {
            event->accept();
            m_owner->clearAttachments();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    static int labelWidth(const QString &label)
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        return metrics.horizontalAdvance(label) + 2 * theme->space4();
    }

    Tile tileFor(const QVariantMap &entry)
    {
        Theme *theme = Theme::instance();
        Tile tile;
        tile.name = entry.value(QStringLiteral("name")).toString();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString data = entry.value(QStringLiteral("data")).toString();
        tile.image = kind == QLatin1String("image")
                     || (kind.isEmpty() && !data.isEmpty());
        if (tile.image) {
            tile.thumb = thumbFor(tile.name, data);
            tile.width = ThumbSide;
            return tile;
        }
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        const int nameWidth = qMin(metrics.horizontalAdvance(tile.name), MaxNameWidth);
        tile.width = 2 * theme->space4() + theme->space6() + theme->space3() + nameWidth;
        return tile;
    }

    QPixmap thumbFor(const QString &name, const QString &data)
    {
        const QString key = name + QLatin1Char('\n') + QString::number(data.size());
        auto cached = m_thumbs.constFind(key);
        if (cached != m_thumbs.cend())
            return cached.value();

        QPixmap thumb;
        QImage image;
        if (image.loadFromData(QByteArray::fromBase64(data.toLatin1())) && !image.isNull()) {
            thumb = QPixmap::fromImage(image.scaled(ThumbSide, ThumbSide, Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
        }
        m_thumbs.insert(key, thumb);
        return thumb;
    }

    void paintThumb(QPainter &painter, const QRect &slot, const Tile &tile) const
    {
        Theme *theme = Theme::instance();
        QPainterPath rounded;
        rounded.addRoundedRect(QRectF(slot), theme->radius(), theme->radius());

        painter.save();
        painter.setClipPath(rounded);
        painter.fillRect(slot, theme->fieldFill());
        if (!tile.thumb.isNull()) {
            const QRect target(slot.center().x() - tile.thumb.width() / 2, slot.y(),
                               tile.thumb.width(), tile.thumb.height());
            painter.drawPixmap(target, tile.thumb);
        } else {
            const QPixmap mark = iconMark(QStringLiteral("image"), theme->space6());
            if (!mark.isNull()) {
                painter.drawPixmap(QRect(slot.center().x() - mark.width() / 2,
                                         slot.center().y() - mark.height() / 2,
                                         mark.width(), mark.height()),
                                   mark);
            }
        }
        painter.restore();

        painter.setPen(QPen(theme->border(), theme->hairline()));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(slot).adjusted(0.5, 0.5, -0.5, -0.5),
                                theme->radius(), theme->radius());
    }

    void paintFile(QPainter &painter, const QRect &slot, const Tile &tile) const
    {
        Theme *theme = Theme::instance();
        const QRect chip(slot.left(), slot.top() + (ThumbSide - FileChipHeight) / 2,
                         slot.width(), FileChipHeight);
        Chrome::paintChip(painter, chip, false, false);

        const QPixmap mark = iconMark(QStringLiteral("file-read"), theme->space6());
        if (!mark.isNull()) {
            painter.drawPixmap(QRect(chip.left() + theme->space4(),
                                     chip.center().y() - mark.height() / 2,
                                     mark.width(), mark.height()),
                               mark);
        }

        const int textLeft = chip.left() + theme->space4() + theme->space6() + theme->space3();
        const QRect textRect(textLeft, chip.top(), chip.right() - theme->space4() - textLeft + 1,
                             chip.height());
        Chrome::paintLine(painter, textRect, Chrome::uiFont(theme->fontData()), theme->dim(),
                          tile.name, Qt::AlignLeft | Qt::AlignVCenter, true);
    }

    Composer *m_owner;
    QVector<Tile> m_tiles;
    QHash<QString, QPixmap> m_thumbs;
    QRect m_clearRect;
    int m_overflow = 0;
    bool m_clearHover = false;
};

// ===========================================================================
// the composer
// ===========================================================================

Composer::Composer(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("composer"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // The QML footer's own air: 10 px above the row and 4 px below it, and 10 px on
    // each side (`composerRow`'s margins, main.qml:1544-1550), with `space4` between
    // the field and the controls that trail it.
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(theme->space5(), theme->space5(), theme->space5(),
                               theme->space2());
    // Zero spacing on purpose: the notice carries the gap above the entry in its
    // own height, so a hidden notice contributes exactly nothing and
    // composerHeight() is arithmetic rather than a guess at the layout's rules for
    // empty items.
    column->setSpacing(0);

    m_notice = new Notice(this);
    column->addWidget(m_notice);

    m_strip = new Strip(this, this);
    m_strip->setVisible(false);
    column->addWidget(m_strip);

    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(theme->space4());

    m_field = new Field(this, this);
    m_entry = m_field->entry();
    row->addWidget(m_field, 1);
    column->addLayout(row);

    // The mode chip: what Enter will do, and the way to change it
    // (main.qml:1792-1816). Its label names the PREFERENCE at all times, which is
    // the QML's own correction: the chip used to read "send" when idle, and the
    // owner lost the setting twice because the choice was invisible until it
    // mattered.
    m_modeChip = new TextButton(QStringLiteral("composerMode"), this);
    m_modeChip->setHandler([this]() {
        m_facade->setBusyEnter(m_facade->busyEnter() == QLatin1String("steer")
                                   ? QStringLiteral("queue")
                                   : QStringLiteral("steer"));
    });
    row->addWidget(m_modeChip, 0, Qt::AlignVCenter);

    // The queue count. The dock's per-item queue rows are stage 4a; while the dock
    // stack is empty this is the one place the composer can say that input is
    // waiting, and its click is the single queue verb the QML offers beside the
    // composer — the queue row's Steer (main.qml:1509-1523).
    m_queueChip = new TextButton(QStringLiteral("composerQueue"), this);
    m_queueChip->setHandler([this]() { steerQueuedHead(); });
    m_queueChip->setVisible(false);
    row->addWidget(m_queueChip, 0, Qt::AlignVCenter);

    // The attach toggle (main.qml:1825-1837). 30 px wide, as the QML states.
    //
    // U+1F4C1 is outside the BMP, so it is two UTF-16 surrogates rather than one
    // QChar: the QML's own comment records that 0x1F4C1 written as `\u1f4c1` came
    // out as a box and a digit, and a QChar constructed from a five-digit value
    // truncates in the same way.
    m_attach = new PushButton(QString(QChar(0xD83D)) + QChar(0xDCC1), 30, this);
    m_attach->setObjectName(QStringLiteral("composerAttach"));
    m_attach->setHandler([this]() { attachRequested(); });
    row->addWidget(m_attach, 0, Qt::AlignVCenter);

    // Send/Stop (main.qml:1839-1855). The button always submits with "queue"
    // (main.qml:1853) — Enter is the control that honours the busyEnter
    // preference; while a turn is running the button is the Stop instead.
    m_send = new PushButton(QStringLiteral("Send"), 0, this);
    m_send->setObjectName(QStringLiteral("composerSend"));
    m_send->setHandler([this]() {
        if (m_facade->busy())
            m_facade->cancel();
        else
            submitWithMode(QStringLiteral("queue"));
    });
    row->addWidget(m_send, 0, Qt::AlignVCenter);

    // ---- what moves the field and the controls ----------------------------
    m_mention = new MentionPopup(this);
    QObject::connect(m_mention, &MentionPopup::chosen, this,
                     [this](const QString &path) { insertMention(path); });

    QObject::connect(m_entry, &QPlainTextEdit::textChanged, this, [this]() {
        // Editing is what reopens a completion list QML closed (main.qml:1691),
        // and here it is what retires a notice about the previous attempt.
        if (!m_notice->text().isEmpty())
            clearNotice();
        relayoutField();
        refreshMention();
    });
    QObject::connect(m_entry, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (m_insertingMention || !m_mention->isOpen())
            return;
        const QString token = mentionToken();
        if (token.isEmpty()) {
            closeMention(true);
            return;
        }
        if (token != m_mentionToken) {
            m_mentionToken = token;
            m_facade->listFileReferences(token);
        }
        showMention();
    });
    QObject::connect(m_entry->document()->documentLayout(),
                     &QAbstractTextDocumentLayout::documentSizeChanged, this,
                     [this]() { relayoutField(); });

    QObject::connect(m_facade, &ClientFacade::fileCandidatesChanged, this, [this]() {
        if (m_mentionToken.isEmpty() || m_mentionDismissed)
            return;
        showMention();
    });

    QObject::connect(qApp, &QApplication::focusChanged, this,
                     [this](QWidget *previous, QWidget *) {
                         if (previous != m_entry || !m_mention->isOpen()
                             || m_mention->underMouse())
                             return;
                         if (QApplication::activeWindow() == m_mention)
                             return;
                         closeMention(true);
                     });

    QObject::connect(m_facade, &ClientFacade::stateChanged, this, [this]() {
        refreshEnabledState();
        refreshPlaceholder();
        if (m_facade->state() != ClientFacade::Ready)
            closeMention(true);
    });
    QObject::connect(m_facade, &ClientFacade::busyChanged, this, [this]() {
        refreshSendButton();
        refreshQueueChip();
    });
    QObject::connect(m_facade, &ClientFacade::busyEnterChanged, this,
                     [this]() { refreshModeChip(); });
    QObject::connect(m_facade, &ClientFacade::pendingSessionChanged, this,
                     [this]() { refreshPlaceholder(); });
    QObject::connect(m_facade, &ClientFacade::sessionIdChanged, this,
                     [this]() { adoptDraft(m_facade->sessionId()); });
    QObject::connect(m_facade, &ClientFacade::queueChanged, this,
                     [this]() { refreshQueueChip(); });
    QObject::connect(m_facade, &ClientFacade::queueCountChanged, this,
                     [this]() { refreshQueueChip(); });
    QObject::connect(m_facade, &ClientFacade::pendingAttachmentsChanged, this,
                     [this]() { refreshAttachments(); });

    // ---- first state ------------------------------------------------------
    m_draftSession = m_facade->sessionId();
    refreshPlaceholder();
    refreshEnabledState();
    refreshModeChip();
    refreshQueueChip();
    refreshSendButton();
    refreshAttachments();
    relayoutField();

    qInfo().nospace() << "composer: field " << m_field->height() << "px (cap "
                      << MaxFieldHeight << "px), placeholder \"" << m_entry->placeholderText()
                      << "\", mode chip \"" << m_modeChip->text() << "\", send \""
                      << (m_facade->busy() ? QStringLiteral("Stop") : QStringLiteral("Send"))
                      << "\"; footer " << composerHeight() << "px";
}

void Composer::focusEntry()
{
    m_entry->setFocus(Qt::OtherFocusReason);
    m_entry->moveCursor(QTextCursor::End);
}

// ---- the keyboard ---------------------------------------------------------

void Composer::onEnterPressed(Qt::KeyboardModifiers modifiers)
{
    // main.qml:1755-1756: Enter takes the primary mode, Ctrl+Enter the
    // complementary one. The mode is resolved at the keystroke, not held from load:
    // a turn that opened since the last key changes what Enter means.
    const QString mode = (modifiers & Qt::ControlModifier) ? alternateMode() : primaryMode();
    submitWithMode(mode);
}

bool Composer::onEscapePressed()
{
    // main.qml:1764-1772: Escape stops the turn, and only a running turn takes the
    // key — otherwise it belongs to the shell.
    if (!m_facade->busy())
        return false;
    m_facade->cancel();
    return true;
}

void Composer::onPasteRequested()
{
    // main.qml:1757-1763. The key is not consumed, so text pastes as usual; the
    // transport says "no image on the clipboard" in the strip when there is none.
    if (m_facade->hasAttachmentStaging())
        m_facade->pasteAttachment();
}

// ---- submission -----------------------------------------------------------

void Composer::submitWithMode(const QString &mode)
{
    // main.qml's submit() (2158-2203), branch for branch.
    //
    // The QML's first branch is `composerReadOnly`, a subagent seat that cannot
    // take input. The facade publishes no lineage read-only flag yet (the strip's
    // parent seat is the only lineage fact it has), so the nearest equivalent is
    // the field's own enablement: nothing can be submitted from a control the
    // window has disabled.
    if (!m_entry->isEnabled())
        return;

    const QString text = m_entry->toPlainText();
    if (text.trimmed().isEmpty())
        return;

    // A bare `/` is the request for the list, not a command and not prose
    // (main.qml:2049). The QML answers with its floating panel; a panel is a popup
    // and the shell's, so this answers in the notice line.
    if (text.trimmed() == QLatin1String("/")) {
        showVocabulary();
        return;
    }

    // A command is not a chat message: it never reaches the model and is never
    // queued or steered, whichever way Enter was pressed (main.qml:2171-2177).
    const QString line = commandLine(text);
    if (!line.isEmpty()) {
        runCommandLine(line);
        return;
    }

    // main.qml:2199-2202: an explicit mode from the caller wins; the text is the
    // field's, untrimmed, and the draft is cleared only after the hand-off.
    m_facade->sendPrompt(text, mode);
    m_entry->clear();
    clearNotice();
}

void Composer::runCommandLine(const QString &line)
{
    const QString word = commandWord(line);
    const QStringList known = m_facade->slashCommands();

    // main.qml:2188-2190: the field is cleared for a name in the transport's own
    // vocabulary, and for none other — a typo is worth correcting rather than
    // retyping. A deferred seat has no session for a command to run in, so
    // nothing is cleared there either; the transport refuses and says why.
    if (!m_facade->pendingSession() && known.contains(word))
        m_entry->clear();

    if (!known.contains(word)) {
        // The refusal, at the point of typing. main.qml hands the line to the
        // transport anyway and lets DshClient's own check produce the sentence
        // (main.qml:2191, DshClient.cpp:7508); the check is the same table, so
        // this says it once, in the place the person is looking, instead of
        // twice — and the transport is never handed a line it would refuse.
        QString message = QStringLiteral("%1 is not a command here").arg(word);
        if (!known.isEmpty())
            message += QStringLiteral(" \u2014 try ") + known.join(QStringLiteral(", "));
        showNotice(message, true);
        qInfo().noquote() << "composer: unknown command refused, draft kept,"
                          << "vocabulary" << known.size() << "names";
        return;
    }

    // main.qml:2191. A known command the transport cannot run — every command on
    // ACP — is refused by the transport itself, as a transcript row and a word in
    // the status (AcpClient.cpp:2027-2041); nothing is invented here to double it.
    m_facade->runCommand(line);
    clearNotice();
}

void Composer::showVocabulary()
{
    const QStringList known = m_facade->slashCommands();
    if (known.isEmpty()) {
        showNotice(QStringLiteral("no commands on this transport"), false);
        return;
    }
    showNotice(QStringLiteral("commands: ") + known.join(QStringLiteral(", ")), false);
    qInfo().noquote() << "composer: listed" << known.size() << "commands in the notice line";
}

void Composer::showNotice(const QString &text, bool error)
{
    m_notice->setNotice(text, error);
    relayoutField();
}

void Composer::clearNotice()
{
    if (m_notice->text().isEmpty())
        return;
    m_notice->setNotice(QString(), false);
    relayoutField();
}

// ---- the mode --------------------------------------------------------------

QString Composer::primaryMode() const
{
    return m_facade->promptMode();
}

QString Composer::alternateMode() const
{
    return m_facade->alternatePromptMode();
}

// ---- the QML's string rules -----------------------------------------------

QString Composer::commandLine(const QString &text)
{
    // main.qml:2058-2067: trim first (a stray space before the slash is a typo,
    // not an escape), then one character is not enough, then a slash, then one
    // line only — a pasted paragraph that opens with a slash is still prose. The
    // newline is looked for after the trim, so one trailing newline still counts
    // as one line while `/a\n/b` does not.
    const QString line = text.trimmed();
    if (line.size() < 2 || !line.startsWith(QLatin1Char('/')))
        return QString();
    if (line.contains(QLatin1Char('\n')))
        return QString();
    return line;
}

QString Composer::commandWord(const QString &line)
{
    // main.qml:2070-2073: `/permission read-only` is `/permission`.
    const int space = line.indexOf(QLatin1Char(' '));
    return space < 0 ? line : line.left(space);
}

// ---- the controls ----------------------------------------------------------

void Composer::refreshEnabledState()
{
    // main.qml:1668, 1834, 1848: the field, the attach toggle and the send button
    // are enabled only when the transport is Ready. Busy does NOT disable the
    // field — typing during a turn is how a message is queued or steered, which is
    // the whole reason busyEnter exists — it turns the send button into Stop
    // (main.qml:1847).
    const bool ready = m_facade->state() == ClientFacade::Ready;
    m_entry->setEnabled(ready);
    m_attach->setEnabled(ready);
    m_send->setEnabled(ready);
    m_field->update();
}

void Composer::refreshPlaceholder()
{
    // main.qml:1625-1629, word for word: the connection first, then a deferred
    // seat (which holds no session to address, so "Message DSH…" would be a
    // message to nowhere), then the ordinary prompt.
    const QString dots(QChar(0x2026));
    const bool ward = m_facade->transportName() == QLatin1String("ward");
    QString placeholder;
    if (m_facade->state() != ClientFacade::Ready)
        placeholder = ward ? QStringLiteral("connecting to the ward daemon") + dots
                           : QStringLiteral("waiting for the ACP server") + dots;
    else if (m_facade->pendingSession())
        placeholder = QStringLiteral("Start a new session") + dots;
    else
        placeholder = ward ? QStringLiteral("Message ward") + dots
                           : QStringLiteral("Message DSH") + dots;

    if (placeholder == m_entry->placeholderText())
        return;
    m_entry->setPlaceholderText(placeholder);
    qInfo().noquote() << "composer placeholder:" << placeholder;
}

void Composer::refreshModeChip()
{
    // main.qml:1802-1814: the preference, not the moment, and it is the same
    // string the QML prints — U+21A9 then the word. Enter uses `promptMode()` (the
    // preference only while a turn is open); the chip names the preference so the
    // choice stays discoverable while idle, which is the QML's own correction.
    const QString word = m_facade->busyEnter() == QLatin1String("steer")
                             ? QStringLiteral("steer")
                             : QStringLiteral("queue");
    m_modeChip->setText(QString(QChar(0x21A9)) + QLatin1Char(' ') + word);
}

void Composer::refreshQueueChip()
{
    const int count = m_facade->queueCount();
    m_queueChip->setVisible(count > 0);
    if (count > 0)
        m_queueChip->setText(QStringLiteral("%1 queued").arg(count));

    // The count is state; the click is the queue row's Steer, and the QML offers
    // that only while a turn is running and only on an item that is not already
    // steered (main.qml:1516). Idle, the queue is about to be delivered anyway and
    // a steer has nothing to steer into, so the chip is a count and says so by not
    // lifting under the pointer.
    m_queueChip->setClickable(m_facade->busy());
}

void Composer::refreshSendButton()
{
    // main.qml:1847: "Stop" while a turn is running, "Send" otherwise.
    m_send->setText(m_facade->busy() ? QStringLiteral("Stop") : QStringLiteral("Send"));
}

void Composer::relayoutField()
{
    if (m_relayingOut)
        return;
    QScopedValueRollback<bool> guard(m_relayingOut, true);

    const int fieldHeight = m_entry->preferredHeight();
    m_field->adoptHeight(fieldHeight);

    // The footer's arithmetic, not a measurement: the field, the margins, the
    // spacing and — only while it is up — the notice's line. The QML makes the
    // same choice for the dock (main.qml:973-1049): the bar decides the space, so
    // it must count it.
    const int height = composerHeight();
    setFixedHeight(height);

    if (fieldHeight != m_loggedFieldHeight) {
        m_loggedFieldHeight = fieldHeight;
        qInfo().nospace() << "composer field: " << fieldHeight << "px (text "
                          << m_entry->textHeight() << "px in " << m_entry->textLines()
                          << " line(s), cap " << MaxFieldHeight << "px), notice "
                          << m_notice->sizeHint().height() << "px, attachments "
                          << (m_strip->isHidden() ? 0 : m_strip->attachmentCount())
                          << ", footer " << height << "px";
    }

    // A keystroke that does not grow the field is not a layout change: the shell
    // would otherwise re-pin the footer and log a line for every character typed.
    if (height != m_announcedHeight) {
        m_announcedHeight = height;
        emit heightChanged();
    }
}

int Composer::composerHeight() const
{
    const QMargins margins = layout()->contentsMargins();
    int height = margins.top() + margins.bottom() + m_field->height();
    if (!m_notice->text().isEmpty())
        height += m_notice->sizeHint().height();
    if (m_strip != nullptr && !m_strip->isHidden())
        height += Strip::stripHeight();
    return height;
}

void Composer::refreshAttachments()
{
    m_strip->refresh();
    relayoutField();
    qInfo().nospace() << "composer attachments: " << m_strip->attachmentCount()
                      << " pending, strip "
                      << (m_strip->isHidden() ? 0 : Strip::stripHeight()) << "px";
}

void Composer::clearAttachments()
{
    const int count = m_facade->pendingAttachments().size();
    if (count == 0)
        return;
    m_facade->clearPendingAttachments();
    qInfo().nospace() << "composer attachments: cleared " << count;
}

// ---- the draft -------------------------------------------------------------

void Composer::adoptDraft(const QString &sessionId)
{
    // main.qml's adoptDraft() (462-499). A draft belongs to the session it was
    // typed in; carrying it into the next one offered text written for another
    // conversation and lost the first one's work.
    const QString next = sessionId;
    if (next == m_draftSession)
        return;

    // An empty previous id means the text in the field belongs to no other
    // session, so the incoming one is the only place it could go: keep it rather
    // than replace it with that session's (empty) draft. This used to require
    // `pendingSession()`, and that was wrong for the transport that needs it
    // most: ACP never reports a pending seat, so on ACP a line typed while the
    // handshake finished was wiped the moment the session id arrived — measured
    // on 2026-10-28, `text 38px in 2 line(s)` six seconds before
    // `restored 0 chars`.
    const bool fromSeat = m_draftSession.isEmpty() && !next.isEmpty()
                          && !m_entry->toPlainText().isEmpty();

    if (!m_draftSession.isEmpty()) {
        if (!m_entry->toPlainText().isEmpty())
            m_drafts.insert(m_draftSession, m_entry->toPlainText());
        else
            m_drafts.remove(m_draftSession);
    }
    m_draftSession = next;

    if (fromSeat && !m_entry->toPlainText().isEmpty()) {
        m_entry->moveCursor(QTextCursor::End);
        qInfo() << "composer draft: kept the seat's text into the session it became";
        return;
    }

    const QString restored = m_drafts.value(next);
    m_entry->setPlainText(restored);
    m_entry->moveCursor(QTextCursor::End);
    qInfo().nospace() << "composer draft: session " << (next.isEmpty() ? "<none>" : next)
                      << ", restored " << restored.size() << " chars";
}

// ---- the two invented-from-the-dock actions --------------------------------

void Composer::steerQueuedHead()
{
    // The dock's queue rows are stage 4a; until they exist, the chip stands in for
    // the head of that list and does exactly what its Steer button does: promote
    // the first item the host has not already steered into the running turn
    // (main.qml:1509-1523). Only while a turn is running, because with the agent
    // idle the host has nothing to steer into and the QML hides the button.
    if (!m_facade->busy())
        return;

    const QVariantList queue = m_facade->queue();
    for (const QVariant &item : queue) {
        const QVariantMap row = item.toMap();
        // A missing `steering` is "delivery unknown", and the QML offers Steer for
        // that too (`steering !== true`, main.qml:1516).
        if (row.value(QStringLiteral("steering")).toBool())
            continue;
        m_facade->updateQueueItem(row.value(QStringLiteral("id")).toString(),
                                  QStringLiteral("steer"));
        return;
    }
}

QString Composer::mentionToken() const
{
    const QTextCursor cursor = m_entry->textCursor();
    if (cursor.hasSelection())
        return QString();
    const QString text = m_entry->toPlainText();
    const int caret = cursor.position();
    if (caret <= 0 || caret > text.size())
        return QString();
    const QString before = text.left(caret);
    const int at = before.lastIndexOf(QLatin1Char('@'));
    if (at < 0)
        return QString();
    if (at > 0 && !before.at(at - 1).isSpace())
        return QString();
    const QString token = before.mid(at + 1);
    for (const QChar character : token) {
        if (character.isSpace())
            return QString();
    }
    return token;
}

void Composer::refreshMention()
{
    if (m_insertingMention)
        return;
    m_mentionDismissed = false;

    const QString token = mentionToken();
    if (token.isEmpty() || m_facade->state() != ClientFacade::Ready) {
        closeMention(true);
        return;
    }

    if (token != m_mentionToken) {
        m_mentionToken = token;
        m_facade->listFileReferences(token);
    }
    showMention();
}

void Composer::showMention()
{
    if (m_mentionToken.isEmpty() || m_mentionDismissed
        || m_facade->state() != ClientFacade::Ready) {
        m_mention->dismiss();
        return;
    }

    const QVariantList candidates = m_facade->fileCandidates();
    if (candidates.isEmpty()) {
        m_mention->dismiss();
        return;
    }
    m_mention->setCandidates(candidates);

    if (QWidget *top = window(); top != nullptr && top != m_watchedWindow) {
        if (m_watchedWindow != nullptr)
            m_watchedWindow->removeEventFilter(this);
        m_watchedWindow = top;
        top->installEventFilter(this);
    }

    const QRect caret = m_entry->cursorRect();
    const QPoint caretTop = m_entry->viewport()->mapToGlobal(caret.topLeft());
    const QPoint fieldTop = m_field->mapToGlobal(QPoint(0, 0));
    m_mention->showAt(QRect(caretTop, QSize(qMax(1, caret.width()), caret.height())),
                      QRect(fieldTop, m_field->size()));
}

void Composer::closeMention(bool clearCandidates)
{
    m_mention->dismiss();
    if (!clearCandidates)
        return;
    m_mentionToken.clear();
    m_mention->setCandidates(QVariantList());
    m_facade->listFileReferences(QString());
}

void Composer::insertMention(const QString &path)
{
    const QString token = mentionToken();
    const int caret = m_entry->textCursor().position();
    const int at = caret - token.size() - 1;
    if (path.isEmpty() || token.isEmpty() || at < 0) {
        closeMention(true);
        return;
    }

    QScopedValueRollback<bool> guard(m_insertingMention, true);
    QTextCursor edit = m_entry->textCursor();
    edit.setPosition(at);
    edit.setPosition(caret, QTextCursor::KeepAnchor);
    edit.insertText(path + QLatin1Char(' '));
    m_entry->setTextCursor(edit);
    closeMention(true);
    m_entry->setFocus(Qt::OtherFocusReason);
    qInfo().noquote() << "composer mention: inserted" << path;
}

bool Composer::onMentionKey(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_mention->isOpen())
        return false;

    switch (key) {
    case Qt::Key_Down:
        m_mention->moveSelection(1);
        return true;
    case Qt::Key_Up:
        m_mention->moveSelection(-1);
        return true;
    case Qt::Key_Escape:
        m_mentionDismissed = true;
        closeMention(false);
        qInfo().noquote() << "composer mention: dismissed, draft kept";
        return true;
    case Qt::Key_Tab:
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (modifiers & Qt::ShiftModifier)
            return false;
        break;
    default:
        return false;
    }

    const QString path = m_mention->selectedPath();
    if (path.isEmpty())
        return false;
    insertMention(path);
    return true;
}

void Composer::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_mention != nullptr)
        m_mention->dismiss();
}

bool Composer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_watchedWindow
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        m_mention->dismiss();
    return QWidget::eventFilter(watched, event);
}

void Composer::attachRequested()
{
    closeMention(true);

    if (!m_facade->hasAttachmentStaging()) {
        showNotice(QStringLiteral("attaching needs the DSH host: this transport stages no files"),
                   false);
        return;
    }

    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Attach files"), QDir::homePath(),
        QStringLiteral("All files (*);;Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.svg)"));

    int staged = 0;
    int unreadable = 0;
    for (const QString &path : chosen) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            ++unreadable;
            continue;
        }
        m_facade->uploadAttachment(QUrl::fromLocalFile(info.absoluteFilePath()));
        ++staged;
    }

    if (unreadable > 0)
        showNotice(QStringLiteral("could not read %1 chosen file(s)").arg(unreadable), true);
    if (!chosen.isEmpty())
        qInfo().nospace() << "composer attach: staged " << staged << " of " << chosen.size()
                          << " chosen file(s)";
}
