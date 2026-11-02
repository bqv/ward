#include "SessionRail.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QAbstractListModel>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// ---- the QML delegate's own numbers ---------------------------------------
//
// Every one of them is `SessionSidebar.qml`'s delegate, named rather than
// inlined because the painter and the hit test have to agree about a rectangle
// or a control is drawn where it cannot be pressed.
constexpr int kIndentBase = 6;      // `indent: 6 + depth * 14`
constexpr int kIndentStep = 14;
constexpr int kMenuButton = 20;     // rowMenuButton width/height
constexpr int kMenuMargin = 4;      // its right margin
constexpr int kCaretWidth = 10;     // the disclosure glyph's column
constexpr int kMarkSize = 16;       // pin.svg/schedule.svg nominal size
constexpr int kTitleTop = 7;        // the Column's `y: 7`
constexpr int kTitleLine = 16;      // max(title, 16 px mark)
constexpr int kHeaderTitleLeft = 31;
constexpr int kLiveChipHeight = 20;

int indentFor(const SessionRailModel::Row &row)
{
    return kIndentBase + row.depth * kIndentStep;
}

QRect menuRectFor(const QRect &row)
{
    const int x = row.x() + row.width() - kMenuMargin - kMenuButton;
    return QRect(x, row.y() + (row.height() - kMenuButton) / 2, kMenuButton, kMenuButton);
}

QRect caretRectFor(const QRect &row)
{
    const QRect menu = menuRectFor(row);
    return QRect(menu.x() - Theme::instance()->space1() - kCaretWidth,
                 row.y() + (row.height() - kTitleLine) / 2, kCaretWidth, kTitleLine);
}

// A header's trailing cluster: the count sits at the right margin, the + keeps
// a reserved slot left of it, and the folded group's live chip sits left of the
// +. The slot is reserved whether or not the + is showing, which is what keeps
// the title from reflowing under the pointer that revealed it.
QRect headerPlusRectFor(const QRect &row, int countWidth)
{
    Theme *theme = Theme::instance();
    const int countLeft = row.x() + row.width() - theme->space3() - countWidth;
    return QRect(countLeft - theme->space2() - kMenuButton,
                 row.y() + (row.height() - kMenuButton) / 2, kMenuButton, kMenuButton);
}

QRect liveChipRectFor(const QRect &row, const QRect &plus, int chipWidth)
{
    Theme *theme = Theme::instance();
    return QRect(plus.x() - theme->space2() - chipWidth,
                 row.y() + (row.height() - kLiveChipHeight) / 2, chipWidth,
                 kLiveChipHeight);
}

QString capRowText(const SessionRailModel::Row &row)
{
    if (row.open)
        return QStringLiteral("Show less");
    const bool subagents = SessionRailModel::capIsSubagents(row.kind);
    if (row.hidden == 1)
        return QStringLiteral("Show 1 more %1")
            .arg(subagents ? QStringLiteral("subagent") : QStringLiteral("session"));
    return QStringLiteral("Show %1 more %2")
        .arg(row.hidden)
        .arg(subagents ? QStringLiteral("subagents") : QStringLiteral("sessions"));
}

/// A 1 px rule in the theme's own hairline. Used only inside the collapsed
/// strip, where it is the same `Theme.borderL1` line the QML column draws.
class Rule : public QWidget
{
public:
    explicit Rule(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(Theme::instance()->hairline());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Theme::instance()->borderL1());
    }
};

} // namespace

// ===========================================================================
// the themed button
// ===========================================================================
//
// `QToolButton` for the contract (click, checkable, tooltip, focus policy) and
// for the plan's own word for the rail's header controls; the appearance is
// this class's, because Fusion's tool button is a bevelled slab and the design
// paints chips (ShellChrome.h). A `QPushButton` would bring the same painted
// look but a different default size and a focus rectangle the rail does not
// want: the rail's controls are pointer targets, and the keyboard belongs to
// the list and the filter.
class SessionRail::Button : public QToolButton
{
public:
    explicit Button(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFixedSize(2 * Theme::instance()->space6() - Theme::instance()->space3(),
                     2 * Theme::instance()->space6() - Theme::instance()->space3());
    }

    /// The collapsed strip's marks are 19 px tall rather than 26: they are one
    /// row each in a 34 px column, and the QML's live badge sets the height.
    void setMarkHeight(int height) { setFixedHeight(height); }

    /// A control whose label is a word rather than a glyph ("New session") keeps
    /// the themed chip but takes its width from the text.
    void setHugText(bool hug)
    {
        m_hugText = hug;
        if (hug) {
            setMinimumWidth(0);
            setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }

    /// The pin the collapsed strip draws is the icon set's own SVG, loaded once
    /// and painted at its nominal 16 px.
    void setIconFile(const QString &path)
    {
        m_icon = QPixmap(path);
        update();
    }

    /// The header's fold glyph is set at the prose size; a count mark at the
    /// data size. One token each, so no number enters the painter.
    void setFontPixels(int pixels) { m_fontPixels = pixels; }

    /// A live badge's number is the rail's state channel, not a label: running
    /// blue or waiting amber. The colour comes from the theme at the call site.
    void setTone(const QColor &colour)
    {
        m_tone = colour;
        m_hasTone = true;
        update();
    }

    QSize sizeHint() const override
    {
        if (!m_hugText)
            return size();
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
        // Two pixels of slack, as the shell header's own chip does: a width of
        // exactly the advance leaves the painter no room and the last letter
        // elides.
        return QSize(metrics.horizontalAdvance(text()) + 2 * theme->space3() + 2,
                     height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        const bool lit = isChecked() || m_active;
        Chrome::paintChip(painter, rect(), underMouse() && isEnabled(), lit);
        if (!m_icon.isNull()) {
            const QPixmap scaled =
                m_icon.scaled(kMarkSize, kMarkSize, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation);
            if (scaled.isNull())
                return;
            painter.drawPixmap(QRect((width() - scaled.width()) / 2,
                                     (height() - scaled.height()) / 2, scaled.width(),
                                     scaled.height()),
                               scaled);
            return;
        }
        const QColor colour = m_hasTone ? m_tone
                                        : (underMouse() || lit ? theme->fg() : theme->dim());
        Chrome::paintLine(painter, rect(),
                          Chrome::uiFont(m_fontPixels == 0 ? theme->fontData() : m_fontPixels),
                          colour, text(), Qt::AlignCenter);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        update();
    }

private:
    QPixmap m_icon;
    QColor m_tone;
    bool m_hasTone = false;
    bool m_hugText = false;
    int m_fontPixels = 0;
    bool m_active = false;
};

// ===========================================================================
// the filter field
// ===========================================================================
//
// `DshTextField.qml`'s recipe: a `fieldFill` well with a `borderL2` hairline,
// the accent ring on focus, `Theme.fontUI` type and `space3` of horizontal
// padding. `QLineEdit` paints its own `Base`, so the base is made transparent
// and the surface is drawn underneath it — that keeps the caret, selection and
// placeholder behaviour of the stock control while the surface belongs to the
// theme.
class SessionRail::Filter : public QLineEdit
{
public:
    explicit Filter(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
        Theme *theme = Theme::instance();
        setObjectName(QStringLiteral("sessionFilter"));
        // The QML's placeholder, verbatim: the ellipsis is U+2026.
        setPlaceholderText(QStringLiteral("Filter title or path\u2026"));
        setFont(Chrome::uiFont(theme->fontUI()));
        setFrame(false);
        setFixedHeight(theme->fieldHeight() - theme->space2());
        setTextMargins(theme->space3(), 0, theme->space3(), 0);
        setClearButtonEnabled(false);

        QPalette fieldPalette = palette();
        fieldPalette.setColor(QPalette::Base, Qt::transparent);
        fieldPalette.setColor(QPalette::Text, theme->fg());
        fieldPalette.setColor(QPalette::PlaceholderText, theme->caption());
        fieldPalette.setColor(QPalette::Highlight, theme->accent());
        fieldPalette.setColor(QPalette::HighlightedText, theme->accentFg());
        setPalette(fieldPalette);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(hasFocus() ? theme->accent() : theme->borderL2(),
                            theme->hairline()));
        painter.setBrush(theme->fieldFill());
        painter.drawRoundedRect(body, theme->radius(), theme->radius());
        QLineEdit::paintEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        // Escape clears the filter, which is the QML's only way back to the
        // full tree from a field this narrow (SessionSidebar.qml:2509).
        if (event->key() == Qt::Key_Escape) {
            clear();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        update();
    }
};

// ===========================================================================
// the rows adapter
// ===========================================================================
//
// The smallest model a `QListView` accepts: it forwards the row count, one
// display string and the accessible line. The paint reads `SessionRailModel::Row`
// directly, so no field is copied into a role and there is no second shape to
// keep in step.
class SessionRail::Rows : public QAbstractListModel
{
public:
    explicit Rows(SessionRail *rail)
        : m_rail(rail)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_rail->m_model.rowCount();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const SessionRailModel::Row *row = m_rail->rowAt(index.row());
        if (row == nullptr)
            return QVariant();
        switch (role) {
        case Qt::DisplayRole:
            return row->kind == SessionRailModel::RowKind::Session ? row->title
                                                                   : capRowText(*row);
        case Qt::AccessibleTextRole: {
            // One line per row for a screen reader: the kind first, because the
            // marks are the one thing a flat string cannot show.
            if (row->kind == SessionRailModel::RowKind::GroupHeader) {
                const QString fold = row->expanded ? QStringLiteral("open")
                                                   : QStringLiteral("folded");
                return QStringLiteral("%1, %2 sessions, %3")
                    .arg(row->title)
                    .arg(row->count)
                    .arg(fold);
            }
            if (SessionRailModel::isCap(row->kind))
                return capRowText(*row);
            QString text = row->title;
            if (!row->sessionId.isEmpty())
                text += QStringLiteral(", ") + row->sessionId;
            if (!row->markWords.isEmpty())
                text += QStringLiteral(", ") + row->markWords;
            if (row->pinned)
                text += QStringLiteral(", pinned");
            if (row->current)
                text += QStringLiteral(", current session");
            return text;
        }
        case Qt::ToolTipRole:
            return row->kind == SessionRailModel::RowKind::Session && !row->markWords.isEmpty()
                       ? row->markWords
                       : QVariant();
        default:
            return QVariant();
        }
    }

    void reload()
    {
        beginResetModel();
        endResetModel();
    }

private:
    SessionRail *m_rail;
};

// ===========================================================================
// the delegate
// ===========================================================================
//
// One row, 45 px, painted from `SessionRailModel::Row`. What it draws is the
// QML delegate's list: the current row's `raised` band and its 3 px accent
// spine, a header's top hairline, the disclosure caret, the row's state mark
// (spinner / idle dot / amber waiting / green unseen), the pin and schedule
// marks, the title line, the `session-…` id line, the live-descendant count and
// the row's ⋯, plus the header's icon, count, + and folded live chip, and the
// cap row's words.
//
// What it deliberately does NOT draw: a per-mark hover surface (the QML's four
// `MouseArea`s and their tooltips are one `Qt::ToolTipRole` here), a visible
// live sheet (the shell has no popover widget yet, so the header chip is not
// drawn at all rather than drawn dead), and the schedule tooltip's count
// sentence (the accessible name carries it). The spinner is a shared timer
// rather than one `RotationAnimator` per row, which is also why a list of a
// thousand rows does not carry a thousand animations.
class SessionRail::Delegate : public QStyledItemDelegate
{
public:
    explicit Delegate(SessionRail *rail)
        : QStyledItemDelegate(rail)
        , m_rail(rail)
    {
        Theme *theme = Theme::instance();
        m_titleFont = Chrome::uiFont(theme->fontUI());
        m_idFont = Chrome::monoFont(theme->fontData() - 1);
        m_headerFont = Chrome::uiFont(theme->fontData());
        m_headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
        m_countFont = Chrome::uiFont(theme->fontData());
        m_capFont = Chrome::uiFont(theme->fontData());
        m_markFont = Chrome::uiFont(theme->fontData());
        m_glyphFont = Chrome::uiFont(theme->fontProse());

        if (!m_rail->m_iconDir.isEmpty()) {
            m_pinIcon = QPixmap(m_rail->m_iconDir + QStringLiteral("/pin.svg"));
            m_scheduleIcon =
                QPixmap(m_rail->m_iconDir + QStringLiteral("/schedule.svg"));
        }
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        Theme *theme = Theme::instance();
        const SessionRailModel::Row *row = m_rail->rowAt(index.row());
        if (row == nullptr)
            return;

        const QRect r = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        const bool focusHere = m_rail->m_list != nullptr && m_rail->m_list->hasFocus();
        const bool hovered = m_rail->m_hoveredRow == index.row();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        if (row->current || selected || (hovered && row->kind != SessionRailModel::RowKind::GroupHeader))
            painter->fillRect(r, theme->raised());
        if (row->current)
            painter->fillRect(QRect(r.x(), r.y(), 3, r.height()), theme->accent());
        if (row->kind == SessionRailModel::RowKind::GroupHeader)
            painter->fillRect(QRect(r.x(), r.y(), r.width(), theme->hairline()),
                              theme->borderL1());
        // The keyboard's row, ringed rather than filled differently: the fill is
        // the followed session's own state, and a selection that replaced it
        // would make "where am I" and "what am I reading" the same question.
        if (selected && focusHere) {
            painter->setPen(QPen(theme->accent(), theme->hairline()));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
        }

        switch (row->kind) {
        case SessionRailModel::RowKind::GroupHeader:
            paintHeader(painter, r, *row, hovered);
            break;
        case SessionRailModel::RowKind::GroupMore:
        case SessionRailModel::RowKind::GroupMoreSubagents:
        case SessionRailModel::RowKind::ChildMore:
        case SessionRailModel::RowKind::ChildMoreSubagents:
            paintCap(painter, r, *row);
            break;
        case SessionRailModel::RowKind::Session:
            paintSession(painter, r, *row, index.row(), hovered);
            break;
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        const SessionRailModel::Row *row = m_rail->rowAt(index.row());
        return QSize(0, row != nullptr && SessionRailModel::isCap(row->kind)
                            ? SessionRail::capRowHeight()
                            : SessionRail::rowHeight());
    }

    /// The clicks the view must not also treat as "open this row": a header (a
    /// fold), a header's + (a new session in that Workspace), a cap row (the
    /// other fold) and the two per-row controls (the caret and the ⋯). A plain
    /// session row returns false, so the view's `clicked` signal opens it.
    bool editorEvent(QEvent *event, QAbstractItemModel *, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() != QEvent::MouseButtonRelease)
            return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        const SessionRailModel::Row *row = m_rail->rowAt(index.row());
        if (row == nullptr)
            return false;

        const QRect r = option.rect;
        const QPoint pos = mouse->pos();

        if (row->kind == SessionRailModel::RowKind::GroupHeader) {
            const int countWidth =
                QFontMetrics(m_countFont).horizontalAdvance(QString::number(row->count));
            const QRect plus = headerPlusRectFor(r, countWidth);
            if (row->workspace && plus.contains(pos)) {
                emit m_rail->sessionNewInWorkspaceRequested(row->groupKey);
                return true;
            }
            m_rail->toggleGroup(row->groupKey);
            return true;
        }
        if (SessionRailModel::isCap(row->kind)) {
            if (SessionRailModel::isChildCap(row->kind)) {
                m_rail->toggleChildFold(row->parentKey,
                                        SessionRailModel::capIsSubagents(row->kind));
            } else {
                m_rail->setGroupOpened(row->groupKey,
                                       SessionRailModel::capIsSubagents(row->kind),
                                       !row->open);
            }
            return true;
        }

        if (row->hasChildren && caretRectFor(r).contains(pos)) {
            m_rail->toggleSessionFold(row->sessionId);
            return true;
        }
        if (m_rail->m_menuSessionId == row->sessionId
            || m_rail->m_hoveredRow == index.row() || row->current) {
            if (menuRectFor(r).contains(pos)) {
                m_rail->requestRowMenu(index.row());
                return true;
            }
        }
        return false;
    }

private:
    const QPixmap &markIcon(const QString &file) const
    {
        if (file.isEmpty())
            return m_nullIcon;
        auto it = m_markIcons.find(file);
        if (it == m_markIcons.end()) {
            QPixmap loaded(m_rail->m_iconDir + QLatin1Char('/') + file);
            it = m_markIcons.insert(file, loaded);
        }
        return it.value();
    }

    void paintHeader(QPainter *painter, const QRect &r,
                     const SessionRailModel::Row &row, bool hovered) const
    {
        Theme *theme = Theme::instance();

        // The disclosure glyph, at `space3` from the left.
        const QRect caret(r.x() + theme->space3(), r.y(), 12, r.height());
        Chrome::paintLine(*painter, caret, m_markFont, theme->faint(),
                          row.expanded ? QStringLiteral("\u25be")
                                       : QStringLiteral("\u25b8"),
                          Qt::AlignLeft | Qt::AlignVCenter);

        // The kind mark: a Workspace is a directory, a schedule is a kind of
        // session, Ungrouped belongs to no directory (Row::icon).
        const QRect icon(r.x() + theme->space3() + 13, r.y() + (r.height() - kMarkSize) / 2,
                         kMarkSize, kMarkSize);
        const QPixmap &mark = markIcon(row.iconFile);
        if (!mark.isNull()) {
            painter->drawPixmap(icon, mark);
        } else {
            Chrome::paintLine(*painter, QRect(icon.x(), r.y(), 14, r.height()), m_markFont,
                              theme->faint(), row.icon, Qt::AlignLeft | Qt::AlignVCenter);
        }

        const int countWidth =
            QFontMetrics(m_countFont).horizontalAdvance(QString::number(row.count));
        const QRect countRect(r.x() + r.width() - theme->space3() - countWidth, r.y(),
                              countWidth, r.height());
        const QRect plus = headerPlusRectFor(r, countWidth);

        const bool liveShown = !row.expanded && row.groupLiveCount > 0;
        const QString liveText = QStringLiteral("%1 live").arg(row.groupLiveCount);
        const int chipWidth =
            QFontMetrics(m_markFont).horizontalAdvance(liveText) + 2 * theme->space2();
        const QRect chip = liveChipRectFor(r, plus, chipWidth);

        const int titleLeft = r.x() + theme->space3() + kHeaderTitleLeft;
        const int titleRight = (liveShown ? chip.x() : plus.x()) - theme->space2();
        Chrome::paintLine(*painter, QRect(titleLeft, r.y(), qMax(0, titleRight - titleLeft),
                                         r.height()),
                          m_headerFont, theme->dim(), row.title,
                          Qt::AlignLeft | Qt::AlignVCenter, true);

        Chrome::paintLine(*painter, countRect, m_countFont, theme->caption(),
                          QString::number(row.count), Qt::AlignLeft | Qt::AlignVCenter);

        // The Workspace's own +: hover-only, as the QML draws it, and never a
        // dead slot — the slot is reserved whether or not it shows.
        if (row.workspace && hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme->raised());
            painter->drawRoundedRect(QRectF(plus).adjusted(0.5, 0.5, -0.5, -0.5),
                                     theme->radius(), theme->radius());
            Chrome::paintLine(*painter, plus, m_glyphFont, theme->fg(),
                              QStringLiteral("\u002b"), Qt::AlignCenter);
        }

        if (liveShown) {
            painter->setPen(QPen(theme->borderL1(), theme->hairline()));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5),
                                     theme->radius(), theme->radius());
            Chrome::paintLine(*painter, chip, m_markFont,
                              row.groupLiveRunning ? theme->running() : theme->warn(),
                              liveText, Qt::AlignCenter);
        }
    }

    void paintCap(QPainter *painter, const QRect &r, const SessionRailModel::Row &row) const
    {
        Theme *theme = Theme::instance();
        const int x = r.x() + indentFor(row) + kMarkSize + theme->space3();
        Chrome::paintLine(*painter,
                          QRect(x, r.y(), qMax(0, r.width() - (x - r.x())), r.height()),
                          m_capFont, theme->dim(), capRowText(row),
                          Qt::AlignLeft | Qt::AlignVCenter);
    }

    void paintSession(QPainter *painter, const QRect &r, const SessionRailModel::Row &row,
                      int rowIndex, bool hovered) const
    {
        Theme *theme = Theme::instance();
        const int indent = indentFor(row);

        // ---- the leading state slot -------------------------------------
        //
        // One mark, and the two attention marks REPLACE the idle dot rather than
        // standing beside it, which is the QML's own correction. A running row
        // gets the arc in `running`; the arc's angle is shared, so a list of a
        // thousand rows is not a thousand animations.
        const QRect leading(r.x() + indent, r.y() + 12, 12, 12);
        if (row.archived) {
            if (row.depth > 0)
                Chrome::paintLine(*painter, leading, m_markFont, theme->dim(),
                                  QStringLiteral("\u21b3"), Qt::AlignCenter);
        } else if (row.parked) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme->warn());
            painter->drawEllipse(QRect(leading.x() + 3, leading.y() + 3, 6, 6));
        } else if (row.unseen) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme->ok());
            painter->drawEllipse(QRect(leading.x() + 3, leading.y() + 3, 6, 6));
        } else if (row.running) {
            QPen pen(theme->running(), 1.8);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawArc(QRectF(leading).adjusted(1.4, 1.4, -1.4, -1.4),
                             (90 - m_rail->m_spinAngle) * 16, int(1.35 * 180 * 16));
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme->faint());
            painter->drawEllipse(QRect(leading.x() + 3, leading.y() + 3, 5, 5));
        }

        // ---- the trailing cluster ---------------------------------------
        const QRect menu = menuRectFor(r);
        const QRect caret = caretRectFor(r);
        const int live = row.liveDescendants;
        const QString liveText = QString::number(live);
        const int liveWidth =
            live > 0 ? QFontMetrics(m_markFont).horizontalAdvance(liveText) : 0;
        const QRect liveRect(caret.x() - theme->space1() - liveWidth,
                             r.y() + (r.height() - 16) / 2, liveWidth, 16);
        const int titleRight =
            (row.hasChildren ? (live > 0 ? liveRect.x() : caret.x()) : caret.x())
            - theme->space2();

        // ---- the title line: the marks, then the title -------------------
        const int markCount = (row.pinned ? 1 : 0) + (row.scheduledTaskCount > 0 ? 1 : 0);
        const int marksWidth = markCount * kMarkSize
                               + (markCount > 1 ? theme->space1() : 0);
        const int textX = r.x() + indent + 16;
        const int titleY = r.y() + kTitleTop;
        int markX = textX;
        if (row.pinned) {
            if (!m_pinIcon.isNull()) {
                painter->drawPixmap(QRect(markX, titleY, kMarkSize, kMarkSize), m_pinIcon);
            } else {
                // The icon is the set's own SVG; if it is missing the row still
                // says "pinned" rather than drawing nothing at all.
                Chrome::paintLine(*painter, QRect(markX, titleY, kMarkSize, kMarkSize),
                                  m_markFont, theme->dim(), QStringLiteral("\u25c6"),
                                  Qt::AlignCenter);
            }
            markX += kMarkSize + (row.scheduledTaskCount > 0 ? theme->space1() : 0);
        }
        if (row.scheduledTaskCount > 0) {
            if (!m_scheduleIcon.isNull()) {
                painter->drawPixmap(QRect(markX, titleY, kMarkSize, kMarkSize),
                                    m_scheduleIcon);
            } else {
                Chrome::paintLine(*painter, QRect(markX, titleY, kMarkSize, kMarkSize),
                                  m_markFont, theme->dim(), QStringLiteral("\u25cb"),
                                  Qt::AlignCenter);
            }
        }

        const int titleX = textX + (marksWidth > 0 ? marksWidth + theme->space1() : 0);
        Chrome::paintLine(*painter,
                          QRect(titleX, titleY, qMax(0, titleRight - titleX), kTitleLine),
                          m_titleFont,
                          row.archived ? theme->caption() : theme->fg(), row.title,
                          Qt::AlignLeft | Qt::AlignVCenter);

        // ---- the id line -------------------------------------------------
        //
        // The session's identity, not its directory: every row in a group shares
        // one cwd, and the id is what "which session" means. Middle-elided
        // because the uuid is the useful part and both ends are fixed.
        const QRect idRect(textX, titleY + kTitleLine + theme->space1(),
                           qMax(0, titleRight - textX),
                           r.height() - kTitleTop - kTitleLine - theme->space1());
        Chrome::paintLine(*painter, idRect, m_idFont, theme->caption(), row.sessionId,
                          Qt::AlignLeft | Qt::AlignTop, true);

        // ---- the trailing controls --------------------------------------
        if (live > 0) {
            Chrome::paintLine(*painter, liveRect, m_markFont,
                              row.descendantRunning ? theme->running()
                              : row.descendantWaiting ? theme->warn()
                                                      : theme->dim(),
                              liveText, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (row.hasChildren) {
            // Expanded is derived, not stored: the model only emits a parent's
            // children when the caret is open, so the row below this one answers
            // it without a second copy of the fold.
            bool open = false;
            const SessionRailModel::Row *next = m_rail->rowAt(rowIndex + 1);
            if (next != nullptr) {
                open = (next->kind == SessionRailModel::RowKind::Session
                        && next->parentId == row.sessionId)
                       || (SessionRailModel::isChildCap(next->kind)
                           && next->parentKey == row.sessionId);
            }
            Chrome::paintLine(*painter, caret, m_markFont, theme->dim(),
                              open ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"),
                              Qt::AlignCenter);
        }
        const bool menuShown =
            hovered || row.current || m_rail->m_menuSessionId == row.sessionId;
        if (menuShown) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme->raised());
            painter->drawRoundedRect(QRectF(menu).adjusted(0.5, 0.5, -0.5, -0.5),
                                     theme->radius(), theme->radius());
            // U+22EF, the horizontal ellipsis at MATHS height: it centres
            // itself, where U+2026 sits on the baseline and reads as a comma run.
            Chrome::paintLine(*painter, menu, m_glyphFont, theme->dim(),
                              QStringLiteral("\u22ef"), Qt::AlignCenter);
        }
    }

    SessionRail *m_rail;
    QFont m_titleFont;
    QFont m_idFont;
    QFont m_headerFont;
    QFont m_countFont;
    QFont m_capFont;
    QFont m_markFont;
    QFont m_glyphFont;
    QPixmap m_pinIcon;
    QPixmap m_scheduleIcon;
    mutable QHash<QString, QPixmap> m_markIcons;
    QPixmap m_nullIcon;
};

// ===========================================================================
// the collapsed strip's mark column
// ===========================================================================
//
// At 34 px no row is drawn, so the strip carries what the rows would have said:
// a pinned mark and one live badge per group holding work, from the model's own
// `liveGroups()` / `pinnedCount()`. The QML draws the same column, with the same
// wording in the tooltip ("<group> · N live") and the same running/warn colour.
class SessionRail::CollapsedMarks : public QWidget
{
public:
    explicit CollapsedMarks(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        m_box = new QVBoxLayout(this);
        m_box->setContentsMargins(0, 0, 0, 0);
        m_box->setSpacing(Theme::instance()->space1());
    }

    QVBoxLayout *box() const { return m_box; }

private:
    QVBoxLayout *m_box;
};

// ===========================================================================
// the rail
// ===========================================================================

int SessionRail::expandedWidth() { return 240; }
int SessionRail::collapsedWidth() { return 34; }
int SessionRail::animationMs() { return 140; }
int SessionRail::rowHeight() { return 45; }
int SessionRail::capRowHeight() { return Theme::instance()->rowHeight(); }

SessionRail::SessionRail(ClientFacade *facade, const QString &iconDir, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
    , m_iconDir(iconDir)
    , m_rowsModel(new Rows(this))
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("sessionRail"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    m_stack = new QStackedWidget(this);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(m_stack);

    // ---- the 240 px shape ------------------------------------------------
    m_expandedPage = new QWidget(m_stack);
    auto *page = new QVBoxLayout(m_expandedPage);
    page->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                             theme->space4());
    page->setSpacing(theme->space4());

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    // 4, not 6: the QML's own note says the larger gap costs this row the
    // overflow control on 224 px of usable rail (SessionSidebar.qml:2117).
    header->setSpacing(theme->space2());

    m_foldButton = new Button(m_expandedPage);
    m_foldButton->setObjectName(QStringLiteral("sessionToggle"));
    m_foldButton->setText(QStringLiteral("\u276e"));
    m_foldButton->setFontPixels(theme->fontProse());
    m_foldButton->setToolTip(QStringLiteral("Fold the session rail"));
    header->addWidget(m_foldButton);

    m_title = new QLabel(QStringLiteral("Sessions"), m_expandedPage);
    m_title->setFont(Chrome::uiFont(theme->fontProse(), true));
    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::WindowText, theme->fg());
    m_title->setPalette(titlePalette);
    header->addWidget(m_title);

    header->addStretch(1);

    m_count = new QLabel(m_expandedPage);
    m_count->setFont(Chrome::uiFont(theme->fontData()));
    QPalette countPalette = m_count->palette();
    countPalette.setColor(QPalette::WindowText, theme->dim());
    m_count->setPalette(countPalette);
    header->addWidget(m_count);

    // The + is a WORKSPACE, not a session: the rail's controls should say what
    // they make (SessionSidebar.qml:2190), and New Session lives in the empty
    // state and the overflow menu.
    m_workspaceNewButton = new Button(m_expandedPage);
    m_workspaceNewButton->setObjectName(QStringLiteral("workspaceNew"));
    m_workspaceNewButton->setText(QStringLiteral("\u002b"));
    m_workspaceNewButton->setFontPixels(theme->fontProse());
    m_workspaceNewButton->setToolTip(QStringLiteral("New workspace"));
    header->addWidget(m_workspaceNewButton);

    m_overflowButton = new Button(m_expandedPage);
    m_overflowButton->setObjectName(QStringLiteral("sessionOverflow"));
    m_overflowButton->setText(QStringLiteral("\u22ee"));
    m_overflowButton->setFontPixels(theme->fontProse());
    m_overflowButton->setCheckable(true);
    m_overflowButton->setToolTip(QStringLiteral("Session rail options"));
    header->addWidget(m_overflowButton);

    page->addLayout(header);

    m_notice = new QLabel(m_expandedPage);
    m_notice->setObjectName(QStringLiteral("sessionRailNotice"));
    m_notice->setFont(Chrome::uiFont(theme->fontData()));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    page->addWidget(m_notice);

    m_filter = new Filter(m_expandedPage);
    page->addWidget(m_filter);

    m_list = new QListView(m_expandedPage);
    m_list->setObjectName(QStringLiteral("sessionList"));
    m_list->setModel(m_rowsModel);
    m_list->setItemDelegate(new Delegate(this));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSpacing(theme->space1());
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::StrongFocus);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(QStringLiteral("Sessions"));
    m_list->viewport()->setMouseTracking(true);
    // The list's own surface is the rail's panel, not the palette's field fill:
    // a row band has to read against the same surface the header does.
    QPalette listPalette = m_list->palette();
    listPalette.setColor(QPalette::Base, theme->panel());
    listPalette.setColor(QPalette::Window, theme->panel());
    m_list->setPalette(listPalette);
    m_list->viewport()->setPalette(listPalette);
    Chrome::styleScrollBar(m_list->verticalScrollBar());
    page->addWidget(m_list, 1);

    // The empty state is drawn over the (empty) viewport, as the QML anchors it
    // to the top of the list: a child of the viewport stays where it is put,
    // because rows are painted rather than re-parented.
    m_emptyLabel = new QLabel(m_list->viewport());
    m_emptyLabel->setObjectName(QStringLiteral("sessionEmpty"));
    m_emptyLabel->setFont(Chrome::uiFont(theme->fontUI()));
    m_emptyLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_emptyLabel->setWordWrap(true);
    QPalette emptyPalette = m_emptyLabel->palette();
    emptyPalette.setColor(QPalette::WindowText, theme->dim());
    m_emptyLabel->setPalette(emptyPalette);
    m_emptyLabel->setVisible(false);

    m_emptyNewButton = new Button(m_list->viewport());
    m_emptyNewButton->setObjectName(QStringLiteral("sessionNew"));
    m_emptyNewButton->setText(QStringLiteral("New session"));
    m_emptyNewButton->setHugText(true);
    m_emptyNewButton->setFontPixels(theme->fontUI());
    m_emptyNewButton->setFixedHeight(theme->controlHeight());
    m_emptyNewButton->setToolTip(QStringLiteral("Start a new session"));
    m_emptyNewButton->setVisible(false);

    // ---- the 34 px strip -------------------------------------------------
    m_collapsedPage = new QWidget(m_stack);
    auto *strip = new QVBoxLayout(m_collapsedPage);
    strip->setContentsMargins(theme->space2(), theme->space4(), theme->space2(),
                              theme->space4());
    strip->setSpacing(theme->space4());

    m_collapsedFoldButton = new Button(m_collapsedPage);
    m_collapsedFoldButton->setObjectName(QStringLiteral("sessionToggleCollapsed"));
    m_collapsedFoldButton->setText(QStringLiteral("\u2630"));
    m_collapsedFoldButton->setFontPixels(theme->fontProse());
    m_collapsedFoldButton->setToolTip(QStringLiteral("Open the session rail"));
    strip->addWidget(m_collapsedFoldButton);

    m_collapsedTerminalButton = new Button(m_collapsedPage);
    m_collapsedTerminalButton->setObjectName(QStringLiteral("terminalToggleCollapsed"));
    m_collapsedTerminalButton->setText(QStringLiteral("\u25b8_"));
    m_collapsedTerminalButton->setToolTip(QStringLiteral("Show the terminal"));
    m_collapsedTerminalButton->setCheckable(true);
    m_collapsedTerminalButton->setVisible(false);
    strip->addWidget(m_collapsedTerminalButton);

    m_collapsedMarks = new CollapsedMarks(m_collapsedPage);
    strip->addWidget(m_collapsedMarks);
    strip->addStretch(1);

    m_stack->addWidget(m_expandedPage);
    m_stack->addWidget(m_collapsedPage);
    m_stack->setCurrentWidget(m_expandedPage);

    // ---- gestures -------------------------------------------------------
    connect(m_foldButton, &QToolButton::clicked, this, &SessionRail::foldToggleRequested);
    connect(m_collapsedFoldButton, &QToolButton::clicked, this,
            &SessionRail::foldToggleRequested);
    connect(m_workspaceNewButton, &QToolButton::clicked, this,
            &SessionRail::workspaceNewRequested);
    connect(m_overflowButton, &QToolButton::clicked, this,
            &SessionRail::overflowMenuRequested);
    connect(m_collapsedTerminalButton, &QToolButton::clicked, this,
            &SessionRail::terminalToggleRequested);
    connect(m_emptyNewButton, &QToolButton::clicked, this,
            &SessionRail::sessionNewRequested);

    connect(m_filter, &QLineEdit::textChanged, this, [this]() { refreshFromFacade(); });
    // The list's keyboard: arrows, Home/End and Page Up/Down are the view's own
    // and stay; Enter, the context-menu key and type-ahead are added here.
    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    connect(m_list, &QAbstractItemView::clicked, this,
            [this](const QModelIndex &index) { openRow(index.row()); });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_list->indexAt(pos);
        if (index.isValid())
            requestRowMenu(index.row());
    });
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        if (m_menuSessionId.isEmpty())
            return;
        m_menuSessionId.clear();
        m_menuRow = -1;
        m_list->viewport()->update();
        emit rowMenuDismissRequested();
    });

    connect(m_facade, &ClientFacade::sessionsChanged, this, &SessionRail::refreshFromFacade);
    connect(m_facade, &ClientFacade::workspacesChanged, this, &SessionRail::refreshFromFacade);
    connect(m_facade, &ClientFacade::sessionIdChanged, this, &SessionRail::refreshFromFacade);
    connect(m_facade, &ClientFacade::unreadSessionIdsChanged, this,
            &SessionRail::refreshFromFacade);
    connect(m_facade, &ClientFacade::pendingInteractionSessionsChanged, this,
            &SessionRail::refreshFromFacade);

    // The transport's own one-line notice, in the same words the QML rail
    // prints. Asked of the transport rather than the facade because the notice
    // is not one of the facade's chrome values.
    if (QObject *transport = m_facade->transport()) {
        connect(transport, SIGNAL(railNotice(QString,bool)), this,
                SLOT(onRailNotice(QString,bool)));
    }

    // The spinner's clock: 12 frames a second, which is the QML animator's 850
    // ms per turn rounded to a cheap timer. It runs only while something is
    // actually running (updateSpinTimer()).
    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(70);
    connect(m_spinTimer, &QTimer::timeout, this, [this]() {
        m_spinAngle = (m_spinAngle + 30) % 360;
        if (m_list != nullptr && m_stack->currentWidget() == m_expandedPage)
            m_list->viewport()->update();
    });

    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    m_noticeTimer->setInterval(8000);
    connect(m_noticeTimer, &QTimer::timeout, this, [this]() { m_notice->setVisible(false); });

    // ---- the model's inputs ---------------------------------------------
    //
    // LAST, once every widget and timer the rebuild touches exists. The folds
    // first: a write is refused until the store has been read, so the first
    // caret click cannot replace every decision ever recorded with the one this
    // window happens to have open (the QML's own guard).
    restoreFolds();
    refreshFromFacade();

    setExpanded(true);
    setStackForWidth(expandedWidth());

    qInfo().nospace() << "session rail: built as a QWidget (widths " << expandedWidth()
                      << "/" << collapsedWidth() << " px, " << animationMs()
                      << " ms transition owned by the shell; filters, header controls"
                         " and the list are the rail's)";
}

SessionRail::~SessionRail() = default;

void SessionRail::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    // The shape follows the WIDTH, not this flag: the shell animates the width
    // and resizeEvent() switches the stack when the animation crosses the
    // threshold. This call only keeps the two fold buttons' tooltips honest.
    m_foldButton->setToolTip(expanded ? QStringLiteral("Fold the session rail")
                                      : QStringLiteral("Open the session rail"));
    setStackForWidth(width());
}

QWidget *SessionRail::overflowAnchor() const
{
    return m_overflowButton;
}

QRect SessionRail::rowGlobalRect(const QString &sessionId) const
{
    // A row exists as a RECTANGLE only while the 240 px tree is up. At 34 px no
    // row is drawn, so there is nothing to anchor a menu to — an invalid rect is
    // the honest answer and the shell refuses to open on it, rather than a menu
    // placed at the collapsed strip's edge for a row the reader cannot see.
    if (m_stack == nullptr || m_stack->currentWidget() != m_expandedPage
        || m_list == nullptr || !m_list->isVisible()) {
        return QRect();
    }
    const int index = indexForKey(QStringLiteral("s:") + sessionId);
    if (index < 0)
        return QRect();
    const QRect local = m_list->visualRect(m_rowsModel->index(index, 0));
    return QRect(m_list->viewport()->mapToGlobal(local.topLeft()), local.size());
}

QAbstractItemView *SessionRail::rowView() const
{
    return m_list;
}

int SessionRail::rowCount() const
{
    return m_model.rowCount();
}

void SessionRail::setTerminalSupported(bool supported)
{
    if (m_terminalSupported == supported)
        return;
    m_terminalSupported = supported;
    m_collapsedTerminalButton->setVisible(!m_expanded && supported);
    rebuildCollapsedMarks();
}

void SessionRail::setTerminalActive(bool active)
{
    if (m_terminalActive == active)
        return;
    m_terminalActive = active;
    m_collapsedTerminalButton->setChecked(active);
    m_collapsedTerminalButton->update();
}

void SessionRail::setSortOrder(SessionRailModel::SortOrder order)
{
    if (m_sortOrder == order)
        return;
    m_sortOrder = order;
    refreshFromFacade();
}

void SessionRail::setGroupByWorkspace(bool grouped)
{
    if (m_groupByWorkspace == grouped)
        return;
    m_groupByWorkspace = grouped;
    refreshFromFacade();
}

void SessionRail::setArchivedFilter(SessionRailModel::ArchivedFilter filter)
{
    if (m_archivedFilter == filter)
        return;
    m_archivedFilter = filter;
    refreshFromFacade();
}

void SessionRail::cycleSortOrder()
{
    setSortOrder(m_sortOrder == SessionRailModel::SortOrder::Updated
                     ? SessionRailModel::SortOrder::Manual
                     : SessionRailModel::SortOrder::Updated);
}

void SessionRail::cycleArchivedFilter()
{
    switch (m_archivedFilter) {
    case SessionRailModel::ArchivedFilter::Default:
        setArchivedFilter(SessionRailModel::ArchivedFilter::Show);
        break;
    case SessionRailModel::ArchivedFilter::Show:
        setArchivedFilter(SessionRailModel::ArchivedFilter::Only);
        break;
    case SessionRailModel::ArchivedFilter::Only:
        setArchivedFilter(SessionRailModel::ArchivedFilter::Default);
        break;
    }
}

void SessionRail::setOverflowMenuOpen(bool open)
{
    m_overflowButton->setChecked(open);
    m_overflowButton->update();
}

void SessionRail::noteRowMenuClosed()
{
    m_menuSessionId.clear();
    m_menuRow = -1;
    if (m_list != nullptr)
        m_list->viewport()->update();
}

void SessionRail::showNotice(const QString &text, bool error)
{
    Theme *theme = Theme::instance();
    m_notice->setText(text);
    QPalette palette = m_notice->palette();
    palette.setColor(QPalette::WindowText, error ? theme->danger() : theme->dim());
    m_notice->setPalette(palette);
    m_notice->setVisible(!text.isEmpty());
    if (!text.isEmpty())
        m_noticeTimer->start();
}

// ---- painting and layout ---------------------------------------------------

void SessionRail::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.fillRect(rect(), theme->panel());
    // The seam against the transcript, a 1 px fill rather than a border: the
    // same hairline the QML draws down the rail's right edge.
    painter.fillRect(QRect(width() - theme->hairline(), 0, theme->hairline(), height()),
                     theme->border());
}

void SessionRail::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    setStackForWidth(width());
    positionEmptyState();
}

void SessionRail::setStackForWidth(int width)
{
    // Half of 240: below this the 26 px controls do not fit an 8 px gutter plus
    // a title, and the QML switches to the strip at the end of its own
    // animation. Switching at the midpoint keeps the in-flight frames legible.
    const bool narrow = width < expandedWidth() / 2;
    QWidget *wanted = narrow ? m_collapsedPage : m_expandedPage;
    if (m_stack->currentWidget() != wanted)
        m_stack->setCurrentWidget(wanted);
    m_collapsedTerminalButton->setVisible(narrow && m_terminalSupported);
}

// ---- the model ------------------------------------------------------------

const SessionRailModel::Row *SessionRail::rowAt(int index) const
{
    if (index < 0 || index >= m_model.rowCount())
        return nullptr;
    return &m_model.rows().at(index);
}

void SessionRail::refreshFromFacade()
{
    SessionRailModel::Inputs inputs;
    inputs.sessions = m_facade->sessions();
    inputs.workspaces = m_facade->workspaces();
    inputs.currentSessionId = m_facade->sessionId();
    inputs.filterText = m_filter != nullptr ? m_filter->text() : QString();
    inputs.sortOrder = m_sortOrder;
    inputs.groupByWorkspace = m_groupByWorkspace;
    inputs.archivedFilter = m_archivedFilter;
    inputs.expandedSessions = m_expandedSessions;
    inputs.collapsedGroups = m_collapsedGroups;
    inputs.openedGroups = m_openedGroups;
    inputs.openedSubagentGroups = m_openedSubagentGroups;
    inputs.openedChildFolds = m_openedChildFolds;
    inputs.openedChildSessionFolds = m_openedChildSessionFolds;
    for (const QVariant &entry : m_facade->pendingInteractionSessions())
        inputs.pendingSessionIds.append(entry.toString());
    for (const QVariant &entry : m_facade->unreadSessionIds())
        inputs.unreadSessionIds.append(entry.toString());

    m_model.setInputs(inputs);
    reloadRows();
}

void SessionRail::reloadRows()
{
    // The reader's place: the row at the top of the viewport and how far its top
    // is above the viewport's, captured before the reset and put back after it.
    // Without this a fold gesture jumps the list to the top, which is the one
    // thing the QML's scroll anchor exists to prevent.
    const QString topKey = rowKeyAt(m_list->indexAt(QPoint(0, 0)).row());
    const int topOffset = topKey.isEmpty() ? 0 : m_list->visualRect(m_list->indexAt(QPoint(0, 0))).top();
    const QString selectedKey = rowKeyAt(m_list->currentIndex().row());

    m_rowsModel->reload();

    if (!selectedKey.isEmpty()) {
        const int selected = indexForKey(selectedKey);
        if (selected >= 0)
            m_list->setCurrentIndex(m_rowsModel->index(selected, 0));
    }
    if (!topKey.isEmpty()) {
        const int top = indexForKey(topKey);
        if (top >= 0) {
            m_list->scrollTo(m_rowsModel->index(top, 0), QAbstractItemView::PositionAtTop);
            // `PositionAtTop` puts the row's top at the viewport's; the offset is
            // what the reader had scrolled past it.
            m_list->verticalScrollBar()->setValue(
                m_list->verticalScrollBar()->value() + topOffset);
        }
    }

    m_filtering = m_model.filtering();
    updateHeader();
    updateEmptyState();
    rebuildCollapsedMarks();
    updateSpinTimer();

    if (m_loggedRows != m_model.rowCount()) {
        m_loggedRows = m_model.rowCount();
        // Built as one sentence rather than streamed piecemeal: QDebug quotes a
        // QString and not a `const char *`, so a mixed stream printed an empty
        // filter as `""` at the end of a line with no filter at all.
        QString line = QStringLiteral("session rail: %1 rows (%2 sessions, %3 after the"
                                      " filter%4), %5 pinned, %6 live groups")
                           .arg(m_loggedRows)
                           .arg(m_model.totalSessionCount())
                           .arg(m_model.filteredSessionCount())
                           .arg(m_filtering ? QStringLiteral(", filter \"%1\"")
                                                  .arg(m_filter->text())
                                            : QString())
                           .arg(m_model.pinnedCount())
                           .arg(m_model.liveGroups().size());
        qInfo().noquote() << line;
    }
}

void SessionRail::updateHeader()
{
    Theme *theme = Theme::instance();
    m_count->setText(m_filtering
                         ? QStringLiteral("%1 / %2").arg(m_model.filteredSessionCount())
                               .arg(m_model.totalSessionCount())
                         : QString::number(m_model.totalSessionCount()));
    QPalette palette = m_count->palette();
    palette.setColor(QPalette::WindowText, m_filtering ? theme->fg() : theme->dim());
    m_count->setPalette(palette);
}

void SessionRail::updateEmptyState()
{
    const bool empty = m_model.rowCount() == 0;
    m_emptyLabel->setText(m_filtering ? QStringLiteral("no session matches")
                                      : QStringLiteral("no sessions"));
    m_emptyLabel->setVisible(empty);
    // The QML offers New Session only when the rail is empty and NOT filtering:
    // under a filter the button would start a session the filter then hides.
    m_emptyNewButton->setVisible(empty && !m_filtering);
    positionEmptyState();
}

void SessionRail::positionEmptyState()
{
    if (m_list == nullptr)
        return;
    const int width = m_list->viewport()->width();
    const int labelWidth = qMax(0, width - 2 * Theme::instance()->space4());
    m_emptyLabel->setFixedWidth(labelWidth);
    m_emptyLabel->move(Theme::instance()->space4(), Theme::instance()->space6());
    m_emptyLabel->setFixedHeight(m_emptyLabel->heightForWidth(labelWidth) > 0
                                     ? m_emptyLabel->heightForWidth(labelWidth)
                                     : m_emptyLabel->sizeHint().height());
    m_emptyLabel->raise();
    m_emptyNewButton->adjustSize();
    m_emptyNewButton->move((width - m_emptyNewButton->width()) / 2,
                           m_emptyLabel->y() + m_emptyLabel->height()
                               + Theme::instance()->space4());
    m_emptyNewButton->raise();
}

void SessionRail::rebuildCollapsedMarks()
{
    Theme *theme = Theme::instance();
    auto *box = m_collapsedMarks->box();
    while (QLayoutItem *item = box->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const bool hasMarks = m_model.pinnedCount() > 0 || !m_model.liveGroups().isEmpty();
    if (!hasMarks)
        return;

    auto *separator = new Rule(m_collapsedMarks);
    box->addWidget(separator);

    if (m_model.pinnedCount() > 0) {
        auto *pin = new Button(m_collapsedMarks);
        pin->setObjectName(QStringLiteral("railPinnedMark"));
        pin->setMarkHeight(theme->fontData() + theme->space3() + theme->hairline());
        pin->setIconFile(m_iconDir + QStringLiteral("/pin.svg"));
        pin->setToolTip(m_model.pinnedCount() == 1
                            ? QStringLiteral("1 pinned session")
                            : QStringLiteral("%1 pinned sessions").arg(m_model.pinnedCount()));
        connect(pin, &QToolButton::clicked, this, &SessionRail::revealFirstPinned);
        box->addWidget(pin);
    }

    for (const SessionRailModel::LiveGroup &group : m_model.liveGroups()) {
        auto *mark = new Button(m_collapsedMarks);
        mark->setObjectName(QStringLiteral("railLiveMark"));
        mark->setMarkHeight(19);
        mark->setText(group.count > 99 ? QStringLiteral("99+")
                                       : QString::number(group.count));
        mark->setTone(group.running ? theme->running() : theme->warn());
        // "live" rather than "running": the count includes the rows parked on a
        // question, and a word that called them running would be the one
        // inaccuracy this mark cannot afford.
        mark->setToolTip(QStringLiteral("%1 \u00b7 %2 live").arg(group.label).arg(group.count));
        const QString key = group.key;
        connect(mark, &QToolButton::clicked, this, [this, key]() { revealLiveGroup(key); });
        box->addWidget(mark);
    }
}

void SessionRail::updateSpinTimer()
{
    if (m_spinTimer == nullptr)
        return;
    bool running = false;
    for (const SessionRailModel::Row &row : m_model.rows()) {
        if (row.kind == SessionRailModel::RowKind::Session && row.running
            && !row.archived) {
            running = true;
            break;
        }
    }
    if (running && !m_spinTimer->isActive())
        m_spinTimer->start();
    else if (!running && m_spinTimer->isActive())
        m_spinTimer->stop();
}

// ---- the folds and their store --------------------------------------------

bool SessionRail::parseEntry(const QString &entry, bool bareIsAFold, QString *key,
                             bool *value, bool *openedOut)
{
    // "<1|0>:<key>" is the row's own fold, "<1|0>o:<key>" the fold beneath it.
    // The flag comes first so the marker sits between it and the colon.
    const bool out = entry.size() > 1 && entry.at(1) == QLatin1Char('o');
    const QChar flag = entry.at(0);
    const int colon = out ? 2 : 1;
    if (entry.size() > colon && entry.at(colon) == QLatin1Char(':')
        && (flag == QLatin1Char('0') || flag == QLatin1Char('1'))) {
        *key = entry.mid(colon + 1);
        *value = flag == QLatin1Char('1');
        *openedOut = out;
        return true;
    }
    if (bareIsAFold) {
        // The oldest shape of the group store: a bare key meant "folded".
        *key = entry;
        *value = true;
        *openedOut = false;
        return true;
    }
    return false;
}

void SessionRail::restoreFolds()
{
    QObject *transport = m_facade->transport();
    QStringList expanded;
    QStringList collapsed;
    bool readExpanded = false;
    bool readCollapsed = false;
    if (transport != nullptr) {
        readExpanded = QMetaObject::invokeMethod(transport, "railExpandedSessions",
                                                 Q_RETURN_ARG(QStringList, expanded));
        readCollapsed = QMetaObject::invokeMethod(transport, "railCollapsed",
                                                  Q_RETURN_ARG(QStringList, collapsed));
    }

    if (readExpanded) {
        for (const QString &entry : expanded) {
            QString key;
            bool value = false;
            bool openedOut = false;
            if (!parseEntry(entry, false, &key, &value, &openedOut) || key.isEmpty())
                continue;
            if (openedOut) {
                if (key.startsWith(QStringLiteral("s:")))
                    m_openedChildSessionFolds.insert(key.mid(2));
                else
                    m_openedChildFolds.insert(key);   // only the open direction is stored
            } else {
                m_expandedSessions.insert(key, value);
            }
        }
    }
    if (readCollapsed) {
        for (const QString &entry : collapsed) {
            QString key;
            bool value = false;
            bool openedOut = false;
            if (!parseEntry(entry, true, &key, &value, &openedOut) || key.isEmpty())
                continue;
            if (openedOut) {
                if (key.startsWith(QStringLiteral("a:")))
                    m_openedSubagentGroups.insert(key.mid(2));
                else
                    m_openedGroups.insert(key);
            } else {
                m_collapsedGroups.insert(key, value);
            }
        }
    }

    // The same policy as the QML: a store that was not read must not be
    // overwritten, or the first caret click replaces every decision ever
    // recorded with the one this window happens to have open. The warning is
    // once per run, because a transport without the surface lacks it always.
    m_foldsRestored = readExpanded && readCollapsed;
    if (!m_foldsRestored && !m_foldWarningSaid) {
        m_foldWarningSaid = true;
        qWarning().nospace()
            << "session rail: this transport has no rail fold store (railExpandedSessions="
            << (readExpanded ? "yes" : "no") << ", railCollapsed="
            << (readCollapsed ? "yes" : "no")
            << "); the rail will not remember its folds after a restart";
    }
    qInfo().nospace() << "session rail: folds restored (" << m_expandedSessions.size()
                      << " carets, " << m_collapsedGroups.size() << " sections, "
                      << m_openedGroups.size() << " open session sections, "
                      << m_openedSubagentGroups.size() << " open subagent sections, "
                      << m_openedChildFolds.size() << " open subagent folds, "
                      << m_openedChildSessionFolds.size() << " open child session folds; store "
                      << (m_foldsRestored ? "read" : "NOT read") << ")";
}

void SessionRail::persistExpansion()
{
    if (!m_foldsRestored) {
        if (!m_foldWarningSaid) {
            m_foldWarningSaid = true;
            qWarning() << "session rail: the fold store was not read at startup;"
                          " not overwriting the stored decisions";
        }
        return;
    }
    QStringList entries;
    for (auto it = m_expandedSessions.constBegin(); it != m_expandedSessions.constEnd(); ++it)
        entries.append((it.value() ? QStringLiteral("1:") : QStringLiteral("0:")) + it.key());
    for (const QString &key : m_openedChildFolds)
        entries.append(QStringLiteral("1o:") + key);
    for (const QString &key : m_openedChildSessionFolds)
        entries.append(QStringLiteral("1o:s:") + key);
    QObject *transport = m_facade->transport();
    if (transport == nullptr)
        return;
    if (!QMetaObject::invokeMethod(transport, "setRailExpandedSessions",
                                   Q_ARG(QStringList, entries))) {
        qWarning() << "session rail: could not save rail expansion;"
                      " the transport has no setRailExpandedSessions()";
    }
}

void SessionRail::persistGroups()
{
    if (!m_foldsRestored) {
        if (!m_foldWarningSaid) {
            m_foldWarningSaid = true;
            qWarning() << "session rail: the fold store was not read at startup;"
                          " not overwriting the stored decisions";
        }
        return;
    }
    QStringList entries;
    for (auto it = m_collapsedGroups.constBegin(); it != m_collapsedGroups.constEnd(); ++it)
        entries.append((it.value() ? QStringLiteral("1:") : QStringLiteral("0:")) + it.key());
    for (const QString &key : m_openedGroups)
        entries.append(QStringLiteral("1o:") + key);
    for (const QString &key : m_openedSubagentGroups)
        entries.append(QStringLiteral("1o:a:") + key);
    QObject *transport = m_facade->transport();
    if (transport == nullptr)
        return;
    if (!QMetaObject::invokeMethod(transport, "setRailCollapsed", Q_ARG(QStringList, entries)))
        qWarning() << "session rail: could not save folded groups;"
                      " the transport has no setRailCollapsed()";
}

void SessionRail::toggleGroup(const QString &key)
{
    // The model's default is "closed unless it holds the current session", so an
    // explicit decision is recorded in both directions: a group the reader
    // opened stays open, and one they closed stays closed.
    const SessionRailModel::Row *header = nullptr;
    for (const SessionRailModel::Row &row : m_model.rows()) {
        if (row.kind == SessionRailModel::RowKind::GroupHeader && row.groupKey == key) {
            header = &row;
            break;
        }
    }
    // The stored value is "collapsed", so it is the OPPOSITE of the fold that is
    // on screen now: open on screen means the click stores "collapsed".
    const bool open = header != nullptr ? header->expanded : true;
    m_collapsedGroups.insert(key, open);
    persistGroups();
    refreshFromFacade();
    // Keep the header under the reader after the row renumbering.
    const int index = indexForKey(QStringLiteral("h:") + key);
    if (index >= 0)
        m_list->setCurrentIndex(m_rowsModel->index(index, 0));
}

void SessionRail::toggleSessionFold(const QString &sessionId)
{
    // Absent means "nobody has said", and the default is open for a parent with
    // children. The first gesture records the opposite of what is on screen, so
    // read the default from the model's own row instead of guessing.
    bool shown = false;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        const SessionRailModel::Row &row = m_model.rows().at(i);
        if (row.kind != SessionRailModel::RowKind::Session || row.sessionId != sessionId)
            continue;
        const SessionRailModel::Row *next = rowAt(i + 1);
        shown = next != nullptr
                && ((next->kind == SessionRailModel::RowKind::Session
                     && next->parentId == sessionId)
                    || (SessionRailModel::isChildCap(next->kind)
                        && next->parentKey == sessionId));
        break;
    }
    m_expandedSessions.insert(sessionId, !shown);
    persistExpansion();
    refreshFromFacade();
}

void SessionRail::toggleChildFold(const QString &parentId, bool subagents)
{
    QSet<QString> &set = subagents ? m_openedChildFolds : m_openedChildSessionFolds;
    if (set.contains(parentId))
        set.remove(parentId);
    else
        set.insert(parentId);
    persistExpansion();
    refreshFromFacade();
    const int index = indexForKey(QStringLiteral("s:") + parentId);
    if (index >= 0)
        m_list->setCurrentIndex(m_rowsModel->index(index, 0));
}

void SessionRail::setGroupOpened(const QString &key, bool subagents, bool opened)
{
    QSet<QString> &set = subagents ? m_openedSubagentGroups : m_openedGroups;
    if (opened)
        set.insert(key);
    else
        set.remove(key);
    persistGroups();
    refreshFromFacade();
    const int index = indexForKey((subagents ? QStringLiteral("ma:") : QStringLiteral("m:"))
                                  + key);
    if (index >= 0)
        m_list->setCurrentIndex(m_rowsModel->index(index, 0));
}

// ---- gestures -------------------------------------------------------------

QString SessionRail::rowKeyAt(int index) const
{
    const SessionRailModel::Row *row = rowAt(index);
    return row != nullptr ? SessionRailModel::rowKey(*row) : QString();
}

int SessionRail::indexForKey(const QString &key) const
{
    if (key.isEmpty())
        return -1;
    for (int i = 0; i < m_model.rowCount(); ++i) {
        if (SessionRailModel::rowKey(m_model.rows().at(i)) == key)
            return i;
    }
    return -1;
}

QString SessionRail::storedTitleFor(const QString &sessionId) const
{
    // The STORED title, because that is what the rename dialog is about to write
    // back: a child's readable catalog label is not the host's title, and
    // offering to replace it would rename the session to its own label.
    for (const QVariant &entry : m_facade->sessions()) {
        const QVariantMap session = entry.toMap();
        if (session.value(QStringLiteral("sessionId")).toString() != sessionId)
            continue;
        const QString title = session.value(QStringLiteral("title")).toString();
        if (!title.isEmpty())
            return title;
        break;
    }
    const int index = indexForKey(QStringLiteral("s:") + sessionId);
    const SessionRailModel::Row *row = rowAt(index);
    return row != nullptr ? row->title : QString();
}

void SessionRail::openRow(int index)
{
    // The hidden list can still hold a focus and a current row after the rail
    // folds; Enter there must not switch the transcript from a row nobody can
    // see.
    if (m_stack->currentWidget() != m_expandedPage)
        return;
    const SessionRailModel::Row *row = rowAt(index);
    if (row == nullptr || row->kind != SessionRailModel::RowKind::Session)
        return;
    // An archived Session is shown for reference, not to be opened: the hosts
    // refuse to resume one and a click that failed would look like a dead row.
    if (row->archived) {
        qInfo().nospace() << "session rail: refused to open archived session "
                          << row->sessionId;
        return;
    }
    emit sessionActivated(row->sessionId, row->parentId, row->child);
}

void SessionRail::requestRowMenu(int index)
{
    // Same rule as openRow(): the collapsed strip draws no rows, so a row menu
    // asked for from a stale focus is refused rather than anchored to an edge.
    if (m_stack->currentWidget() != m_expandedPage) {
        qInfo() << "session rail: no row menu while the rail is collapsed;"
                   " the 34 px strip draws no rows to anchor one to";
        return;
    }
    const SessionRailModel::Row *row = rowAt(index);
    if (row == nullptr || row->kind != SessionRailModel::RowKind::Session)
        return;
    m_menuRow = index;
    m_menuSessionId = row->sessionId;
    m_list->viewport()->update();
    emit rowMenuRequested(rowGlobalRect(row->sessionId), row->sessionId,
                          storedTitleFor(row->sessionId), row->pinned, row->archived);
}

void SessionRail::revealFirstPinned()
{
    // The ROSTER, not the rows: a pinned session inside a folded section has no
    // row at all, and that is exactly the case the collapsed strip's pin mark
    // exists to reach. Looking for it in `rows()` would answer "nothing pinned"
    // precisely when the rail is folded.
    for (const QVariant &entry : m_facade->sessions()) {
        const QVariantMap session = entry.toMap();
        if (!session.value(QStringLiteral("pinned")).toBool())
            continue;
        const QString sessionId = session.value(QStringLiteral("sessionId")).toString();
        if (sessionId.isEmpty())
            continue;
        const QString groupKey = SessionRailModel::groupOf(session);
        // The section is very likely folded (the model's default only opens the
        // one holding the current session), so open it explicitly; the store
        // records both directions, and an explicit `false` is "open".
        m_collapsedGroups.insert(groupKey, false);
        persistGroups();
        refreshFromFacade();
        const int index = indexForKey(QStringLiteral("s:") + sessionId);
        if (index >= 0) {
            m_list->setCurrentIndex(m_rowsModel->index(index, 0));
            m_list->scrollTo(m_rowsModel->index(index, 0), QAbstractItemView::PositionAtTop);
            m_list->setFocus(Qt::ShortcutFocusReason);
        }
        qInfo().nospace() << "session rail: reveal pinned -> " << sessionId
                          << (index >= 0 ? " (row found)" : " (no row)");
        return;
    }
    qInfo() << "session rail: reveal pinned -> no pinned session on the roster";
}

void SessionRail::revealLiveGroup(const QString &key)
{
    m_collapsedGroups.insert(key, false);
    m_openedGroups.insert(key);
    persistGroups();
    refreshFromFacade();
    int index = indexForKey(QStringLiteral("h:") + key);
    if (index < 0)
        index = indexForKey(QStringLiteral("m:") + key);
    if (index >= 0) {
        m_list->setCurrentIndex(m_rowsModel->index(index, 0));
        m_list->scrollTo(m_rowsModel->index(index, 0), QAbstractItemView::PositionAtTop);
        m_list->setFocus(Qt::ShortcutFocusReason);
    }
    qInfo().nospace() << "session rail: reveal live group \"" << key << "\" ("
                      << (index >= 0 ? "row found" : "no row") << ")";
}

// ---- the keyboard ---------------------------------------------------------

bool SessionRail::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentIndex().row();
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            openRow(row);
            return true;
        }
        if (key->key() == Qt::Key_Menu
            || (key->key() == Qt::Key_F10
                && key->modifiers().testFlag(Qt::ShiftModifier))) {
            // The context-menu key, and the Shift+F10 every desktop also accepts.
            if (row >= 0)
                requestRowMenu(row);
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            if (!m_filter->text().isEmpty()) {
                m_filter->clear();
                return true;
            }
            return false;
        }
        // Type-ahead goes through the filter, not the view: the filter is the
        // rail's one search, and a keyboard search in the list would have to
        // duplicate its matching rules. Printable only — arrows, Home/End and
        // Page Up/Down have an empty text and stay the view's.
        const QString typed = key->text();
        if (!typed.isEmpty()
            && !key->modifiers().testFlag(Qt::ControlModifier)
            && !key->modifiers().testFlag(Qt::AltModifier)
            && !key->modifiers().testFlag(Qt::MetaModifier)
            && typed.at(0).isPrint()) {
            m_filter->setFocus(Qt::ShortcutFocusReason);
            m_filter->insert(typed);
            return true;
        }
    }

    if (watched == m_list && (event->type() == QEvent::FocusIn
                              || event->type() == QEvent::FocusOut)) {
        m_list->viewport()->update();
    }

    if (watched == m_list->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const int row = m_list->indexAt(mouse->pos()).row();
            if (row != m_hoveredRow) {
                m_hoveredRow = row;
                m_list->viewport()->update();
            }
            // A pointer over a control that does something gets the hand; the
            // body of a row is a target too, so it keeps the same cursor.
            m_list->viewport()->setCursor(row >= 0 ? Qt::PointingHandCursor
                                                   : Qt::ArrowCursor);
        } else if (event->type() == QEvent::Leave && m_hoveredRow != -1) {
            // The ⋯ would vanish from under the pointer as it crossed the
            // viewport's edge; the QML keeps it while the row's own area or the
            // glyph is under the pointer, which is the same rule here.
            m_hoveredRow = -1;
            m_list->viewport()->update();
        }
    }

    return QWidget::eventFilter(watched, event);
}
