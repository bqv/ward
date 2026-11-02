#include "TrajectoryPage.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractListModel>
#include <QAction>
#include <QActionGroup>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

namespace {

QString laneName(int lane)
{
    switch (lane) {
    case 0:
        return QStringLiteral("Input");
    case 1:
        return QStringLiteral("Model");
    case 2:
        return QStringLiteral("Tools");
    default:
        return QStringLiteral("All lanes");
    }
}

struct LineRanges
{
    QVector<QPair<int, int>> ranges;
    bool more = false;
};

LineRanges layoutText(const QString &text, const QFont &font, int width, int maxLines)
{
    LineRanges result;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    for (;;) {
        if (maxLines > 0 && result.ranges.size() >= maxLines) {
            const QTextLine probe = layout.createLine();
            result.more = probe.isValid();
            break;
        }
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        result.ranges.append(qMakePair(line.textStart(), line.textLength()));
    }
    layout.endLayout();
    return result;
}

int wrappedTextHeight(const QString &text, const QFont &font, int width, int maxLines)
{
    if (text.isEmpty() || width <= 0)
        return 0;
    const LineRanges lines = layoutText(text, font, width, maxLines);
    return int(lines.ranges.size()) * QFontMetrics(font).lineSpacing();
}

}

struct TrajectoryPage::Row
{
    enum Type { Turn, Step, Collapsed, Cell };

    Type type = Turn;
    int turn = 0;
    int step = 0;
    int stepCount = 0;
    int toolCount = 0;
    bool open = false;
    QVariantMap cell;
    QString key;
};

class TrajectoryPage::Rows : public QAbstractListModel
{
public:
    explicit Rows(QObject *parent = nullptr)
        : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const Row *row = rowAt(index.row());
        if (row == nullptr)
            return QVariant();
        if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole)
            return QVariant();
        return accessibleText(*row);
    }

    const Row *rowAt(int index) const
    {
        if (index < 0 || index >= m_rows.size())
            return nullptr;
        return &m_rows.at(index);
    }

    QString keyAt(int index) const
    {
        const Row *row = rowAt(index);
        return row == nullptr ? QString() : row->key;
    }

    int indexOfKey(const QString &key) const
    {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).key == key)
                return i;
        }
        return -1;
    }

    void reload(QVector<Row> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

private:
    QString accessibleText(const Row &row) const
    {
        switch (row.type) {
        case Row::Turn: {
            QString text = row.turn > 0 ? QStringLiteral("Turn %1").arg(row.turn)
                                        : QStringLiteral("Before the first turn");
            if (row.open)
                text += QStringLiteral(", running");
            return text;
        }
        case Row::Step:
            return QStringLiteral("Step %1").arg(row.step);
        case Row::Collapsed:
            return QStringLiteral("%1 steps, %2 tool calls")
                .arg(row.stepCount)
                .arg(row.toolCount);
        case Row::Cell: {
            const QString kind = row.cell.value(QStringLiteral("kind")).toString();
            const QString summary = row.cell.value(QStringLiteral("summary")).toString();
            const QString text = row.cell.value(QStringLiteral("text")).toString();
            QString line = kind;
            if (!text.isEmpty())
                line += QLatin1Char(' ') + text;
            if (!summary.isEmpty())
                line += QStringLiteral(", ") + summary;
            if (line.isEmpty())
                line = QStringLiteral("record %1").arg(row.cell.value(QStringLiteral("seq")).toString());
            return line;
        }
        }
        return QString();
    }

    QVector<Row> m_rows;
};

class TrajectoryPage::ChipButton : public QToolButton
{
public:
    explicit ChipButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        Theme *theme = Theme::instance();
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFont(Chrome::uiFont(theme->fontData()));
        setFixedHeight(theme->controlHeight());
    }

    void setCaret(bool caret)
    {
        m_caret = caret;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(font());
        int width = metrics.horizontalAdvance(text()) + 2 * theme->space5();
        if (m_caret)
            width += theme->space4() + metrics.horizontalAdvance(QStringLiteral("\u25be"));
        return QSize(width, theme->controlHeight());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        Chrome::paintChip(painter, rect(), underMouse(), false);
        const QFontMetrics metrics(font());
        int right = width() - theme->space5();
        if (m_caret) {
            const QRect caretRect(right - metrics.horizontalAdvance(QStringLiteral("\u25be")),
                                  (height() - theme->fontData()) / 2,
                                  metrics.horizontalAdvance(QStringLiteral("\u25be")),
                                  theme->fontData());
            Chrome::paintLine(painter, caretRect, font(), theme->dim(), QStringLiteral("\u25be"),
                              Qt::AlignRight | Qt::AlignVCenter);
            right -= theme->space4() + metrics.horizontalAdvance(QStringLiteral("\u25be"));
        }
        const QRect labelRect(theme->space5(), 0, right - theme->space5(), height());
        Chrome::paintLine(painter, labelRect, font(), underMouse() ? theme->fg() : theme->dim(), text(),
                          Qt::AlignLeft | Qt::AlignVCenter);
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
    bool m_caret = false;
};

class TrajectoryPage::FilterField : public QLineEdit
{
public:
    explicit FilterField(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
        Theme *theme = Theme::instance();
        setFont(Chrome::uiFont(theme->fontUI()));
        setFrame(false);
        setFixedHeight(theme->controlHeight());
        setTextMargins(theme->space3(), 0, theme->space3(), 0);
        setClearButtonEnabled(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

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
        painter.setPen(QPen(hasFocus() ? theme->accent() : theme->borderL2(), theme->hairline()));
        painter.setBrush(theme->fieldFill());
        painter.drawRoundedRect(body, theme->radius(), theme->radius());
        QLineEdit::paintEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
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

class TrajectoryPage::List : public QListView
{
public:
    explicit List(TrajectoryPage *page)
        : QListView(page)
        , m_page(page)
    {
    }

protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        QListView::mouseMoveEvent(event);
        const QModelIndex index = indexAt(event->pos());
        const int row = index.isValid() ? index.row() : -1;
        const int offset = index.isValid() ? event->pos().y() - visualRect(index).top() : 0;
        const int hovered = row >= 0 && m_page->rowBodyContains(row, offset) ? row : -1;
        if (hovered != m_page->m_hoveredRow) {
            m_page->m_hoveredRow = hovered;
            viewport()->update();
        }
        viewport()->setCursor(m_page->rowActivatable(row) && m_page->rowBodyContains(row, offset)
                                  ? Qt::PointingHandCursor
                                  : Qt::ArrowCursor);
    }

    void leaveEvent(QEvent *event) override
    {
        QListView::leaveEvent(event);
        if (m_page->m_hoveredRow != -1) {
            m_page->m_hoveredRow = -1;
            viewport()->update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QListView::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton)
            return;
        const QModelIndex index = indexAt(event->pos());
        if (index.isValid())
            m_page->activateRow(index.row(), event->pos().y() - visualRect(index).top());
    }

private:
    TrajectoryPage *m_page;
};

class TrajectoryPage::LaneStrip : public QWidget
{
public:
    explicit LaneStrip(TrajectoryPage *page)
        : QWidget(page)
        , m_page(page)
    {
        setObjectName(QStringLiteral("trajectoryStrip"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(m_page->stripHeight());
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Jump to a record"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->raised());
        painter.fillRect(QRect(0, height() - theme->hairline(), width(), theme->hairline()),
                         theme->borderL2());

        const int pitch = m_page->lanePitch();
        const int caption = m_page->captionWidth();
        const QFont captionFont = Chrome::monoFont(theme->fontData());
        painter.setFont(captionFont);
        const QVector<int> counts = m_page->laneCounts();
        for (int lane = 0; lane < 3; ++lane) {
            painter.setPen(m_page->laneColour(lane));
            const QRect captionRect(0, theme->space3() + lane * pitch, caption - theme->space4(),
                                    pitch);
            painter.drawText(captionRect, Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1 %2").arg(laneName(lane)).arg(counts.value(lane)));
        }
        painter.fillRect(QRect(caption, theme->space3(), theme->hairline(), 3 * pitch),
                         theme->borderL2());

        const QRect plot = plotRect();
        const int total = int(m_page->m_cells.size());
        if (total == 0 || plot.width() <= 0)
            return;

        const int block = qMax(1, qMin(theme->space4(), plot.width() / total));
        const int gap = block > (theme->space1() + theme->hairline()) ? theme->hairline() : 0;

        painter.setPen(QPen(theme->border(), theme->hairline()));
        int lastTurn = -1;
        for (int i = 0; i < total; ++i) {
            const int turn = m_page->m_cells.at(i).toMap().value(QStringLiteral("turn")).toInt();
            if (i > 0 && turn != lastTurn) {
                const int x = plot.left() + qRound(double(i) / double(total) * double(plot.width()));
                painter.drawLine(x, plot.top(), x, plot.bottom());
            }
            lastTurn = turn;
        }

        for (int i = 0; i < total; ++i) {
            const QVariantMap cell = m_page->m_cells.at(i).toMap();
            const int lane = cell.value(QStringLiteral("lane")).toInt();
            if (lane < 0 || lane > 2)
                continue;
            const QString status = cell.value(QStringLiteral("status")).toString();
            const QColor colour = status == QLatin1String("error")
                                      ? theme->danger()
                                  : status == QLatin1String("running") ? theme->ok()
                                                                       : m_page->laneColour(lane);
            const QRectF blockRect(plot.left() + double(i) / double(total) * double(plot.width())
                                       + double(gap) / 2.0,
                                   plot.top() + lane * pitch + theme->space1(),
                                   qMax(1.0, double(plot.width()) / double(total) - double(gap)),
                                   pitch - 2 * theme->space1());
            painter.fillRect(blockRect, colour);
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QWidget::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton || m_page->m_cells.isEmpty())
            return;
        const QRect plot = plotRect();
        if (!plot.contains(event->pos()) || plot.width() <= 0)
            return;
        const int total = int(m_page->m_cells.size());
        const int index = qBound(0, int((event->pos().x() - plot.left()) * total / plot.width()),
                                 total - 1);
        m_page->jumpToCell(
            m_page->m_cells.at(index).toMap().value(QStringLiteral("id")).toString());
    }

private:
    QRect plotRect() const
    {
        Theme *theme = Theme::instance();
        const int caption = m_page->captionWidth();
        return QRect(caption + theme->hairline(), theme->space3(),
                     width() - caption - theme->hairline() - theme->space4(),
                     3 * m_page->lanePitch());
    }

    TrajectoryPage *m_page;
};

class TrajectoryPage::Delegate : public QStyledItemDelegate
{
public:
    explicit Delegate(TrajectoryPage *page, QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_page(page)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const Row *row = m_page->m_rows->rowAt(index.row());
        if (row == nullptr)
            return;

        painter->save();
        const QRect r = option.rect;
        switch (row->type) {
        case Row::Turn:
            paintTurn(painter, r, *row);
            break;
        case Row::Step:
            paintStep(painter, r, *row);
            break;
        case Row::Collapsed:
            paintCollapsed(painter, r, *row);
            break;
        case Row::Cell:
            paintCell(painter, r, index.row(), row->cell);
            break;
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int width = m_page->m_list != nullptr ? m_page->m_list->viewport()->width()
                                                    : option.rect.width();
        return QSize(0, m_page->rowHeight(index.row(), width));
    }

private:
    void paintTurn(QPainter *painter, const QRect &r, const Row &row) const
    {
        Theme *theme = Theme::instance();
        painter->fillRect(r, theme->panel());
        painter->fillRect(QRect(r.left(), r.top(), r.width(), theme->hairline()),
                          theme->border());
        painter->fillRect(QRect(r.left(), r.top() + r.height() - theme->hairline(), r.width(),
                                theme->hairline()),
                          theme->border());

        const QFont labelFont = Chrome::monoFont(theme->fontData(), true);
        const QFontMetrics metrics(labelFont);
        const QString label = row.turn > 0 ? QStringLiteral("Turn %1").arg(row.turn)
                                           : QStringLiteral("Before the first turn");
        const QRect labelRect(r.left() + theme->space5(), r.top(),
                              metrics.horizontalAdvance(label), r.height());
        Chrome::paintLine(*painter, labelRect, labelFont, theme->fg(), label);

        int x = labelRect.right() + theme->space4();
        if (row.open) {
            const QFont runningFont = Chrome::monoFont(theme->fontData());
            const QRect runningRect(x, r.top(),
                                    QFontMetrics(runningFont).horizontalAdvance(
                                        QStringLiteral("running")),
                                    r.height());
            Chrome::paintLine(*painter, runningRect, runningFont, theme->ok(),
                              QStringLiteral("running"));
        }

        const QString caret = m_page->isTurnCollapsed(row.turn) ? QStringLiteral("\u25b8")
                                                                : QStringLiteral("\u25be");
        Chrome::paintLine(*painter,
                          QRect(r.left(), r.top(), r.width() - theme->space5(), r.height()),
                          Chrome::uiFont(theme->fontData()), theme->dim(), caret,
                          Qt::AlignRight | Qt::AlignVCenter);
    }

    void paintStep(QPainter *painter, const QRect &r, const Row &row) const
    {
        Theme *theme = Theme::instance();
        const int size = 2 * theme->space1();
        painter->fillRect(QRect(r.left() + theme->space5(),
                                r.top() + (r.height() - size) / 2, size, size),
                          theme->faint());
        const int x = r.left() + theme->space5() + size + theme->space4();
        Chrome::paintLine(*painter, QRect(x, r.top(), r.width() - x, r.height()),
                          Chrome::monoFont(theme->fontData()), theme->dim(),
                          QStringLiteral("Step %1").arg(row.step));
    }

    void paintCollapsed(QPainter *painter, const QRect &r, const Row &row) const
    {
        Theme *theme = Theme::instance();
        const int x = r.left() + theme->space5() + 2 * theme->space1() + theme->space4();
        Chrome::paintLine(*painter, QRect(x, r.top(), r.width() - x, r.height()),
                          Chrome::monoFont(theme->fontData()), theme->dim(),
                          QStringLiteral("\u2026 %1 step(s) \u00b7 %2 tool call(s)")
                              .arg(row.stepCount)
                              .arg(row.toolCount));
    }

    void paintCell(QPainter *painter, const QRect &r, int index, const QVariantMap &cell) const
    {
        Theme *theme = Theme::instance();
        const QString id = cell.value(QStringLiteral("id")).toString();
        const int lane = cell.value(QStringLiteral("lane")).toInt();
        const QString kind = cell.value(QStringLiteral("kind")).toString();
        const QString status = cell.value(QStringLiteral("status")).toString();
        const bool error = status == QLatin1String("error");
        const bool running = status == QLatin1String("running");
        const bool expanded = m_page->isCellExpanded(id);
        const bool expandable = m_page->cellExpandable(cell);

        if (m_page->m_hoveredRow == index && expandable)
            painter->fillRect(r, theme->raised());

        painter->fillRect(QRect(r.left(), r.top(), theme->space1() + theme->hairline(),
                                r.height()),
                          expanded ? theme->accent() : m_page->laneColour(lane));
        painter->fillRect(QRect(r.left(), r.top() + r.height() - theme->hairline(), r.width(),
                                theme->hairline()),
                          theme->border());

        const int bodyTop = r.top() + theme->space3();
        const int bodyHeight = m_page->bodyHeight(cell, r.width());
        const QFont dataFont = Chrome::monoFont(theme->fontData());

        const int kindLeft = r.left() + theme->space5();
        const QRect kindRect(kindLeft, bodyTop, m_page->kindSlot(), bodyHeight);
        painter->save();
        painter->setFont(dataFont);
        painter->setPen(error ? theme->danger() : theme->dim());
        painter->drawText(kindRect, Qt::AlignRight | Qt::AlignVCenter, kind.toUpper());
        painter->restore();

        Chrome::paintLine(*painter,
                          QRect(kindLeft + m_page->kindSlot() + theme->space3(), bodyTop,
                                m_page->seqColumn(), bodyHeight),
                          dataFont, theme->dim(),
                          QStringLiteral("#%1").arg(cell.value(QStringLiteral("seq")).toString()));

        const int statusRight = r.left() + r.width() - 2 * theme->space5() - theme->space3();
        Chrome::paintLine(*painter, QRect(r.left(), bodyTop, statusRight - r.left(), bodyHeight),
                          dataFont, error ? theme->danger() : (running ? theme->ok() : theme->dim()),
                          status, Qt::AlignRight | Qt::AlignVCenter);

        const QRect caretRect(r.left() + r.width() - 2 * theme->space5(), bodyTop,
                              theme->space5(), bodyHeight);
        Chrome::paintLine(*painter, caretRect, Chrome::uiFont(theme->fontData()), theme->dim(),
                          expandable ? (expanded ? QStringLiteral("\u25be")
                                                 : QStringLiteral("\u25b8"))
                                     : QString(),
                          Qt::AlignCenter);

        const int contentLeft = r.left() + m_page->contentLeft();
        const int contentWidth =
            r.left() + m_page->contentRight(cell, r.width()) - contentLeft;
        const bool machine = kind == QLatin1String("tool") || kind == QLatin1String("subtool");
        const int maxLines = m_page->maxTextLines(expanded);

        int y = bodyTop;
        const QString summary = m_page->summaryText(cell);
        if (!summary.isEmpty()) {
            const QFont summaryFont = machine ? Chrome::monoFont(theme->fontMono())
                                              : Chrome::uiFont(theme->fontProse());
            const int height =
                m_page->wrappedHeight(summary, summaryFont, contentWidth, maxLines);
            paintWrapped(painter, QRect(contentLeft, y, contentWidth, height), summaryFont,
                         theme->fg(), summary, maxLines);
            y += height;
        }

        const QString result = m_page->resultText(cell);
        if (!result.isEmpty()) {
            if (y > bodyTop)
                y += theme->space1();
            const QFont resultFont = Chrome::monoFont(theme->fontData());
            const int height = m_page->wrappedHeight(result, resultFont, contentWidth, maxLines);
            paintWrapped(painter, QRect(contentLeft, y, contentWidth, height), resultFont,
                         error ? theme->danger() : theme->dim(), result, maxLines);
        }

        if (!expanded)
            return;

        const int detailLeft = r.left() + m_page->contentLeft();
        const int detailWidth = r.left() + r.width() - theme->space5() - detailLeft;
        QFont labelFont = Chrome::monoFont(theme->fontData());
        labelFont.setLetterSpacing(QFont::AbsoluteSpacing, theme->space1() / 4.0);
        const QFont bodyFont = Chrome::monoFont(theme->fontMono());
        const int labelHeight = QFontMetrics(labelFont).height();
        const int bodyWidth = detailWidth - 2 * theme->space5();

        int detailY = bodyTop + bodyHeight + theme->space3();
        const QVector<QPair<QString, QString>> blocks{
            {QStringLiteral("Thinking"), cell.value(QStringLiteral("thinking")).toString()},
            {QStringLiteral("Input"), cell.value(QStringLiteral("input")).toString()},
            {QStringLiteral("Output"), cell.value(QStringLiteral("output")).toString()},
        };
        bool first = true;
        for (const QPair<QString, QString> &block : blocks) {
            if (block.second.isEmpty())
                continue;
            if (!first)
                detailY += theme->space4();
            first = false;

            Chrome::paintLine(*painter, QRect(detailLeft, detailY, detailWidth, labelHeight),
                              labelFont, theme->dim(), block.first.toUpper());
            detailY += labelHeight + theme->space2();

            const int textHeight =
                m_page->wrappedHeight(block.second, bodyFont, bodyWidth, 0);
            painter->fillRect(QRect(detailLeft, detailY, detailWidth,
                                    textHeight + 2 * theme->space4()),
                              theme->raised());
            paintWrapped(painter,
                         QRect(detailLeft + theme->space5(), detailY + theme->space4(), bodyWidth,
                               textHeight),
                         bodyFont, theme->dim(), block.second, 0);
            detailY += textHeight + 2 * theme->space4();
        }
    }

    void paintWrapped(QPainter *painter, const QRect &rect, const QFont &font,
                      const QColor &colour, const QString &text, int maxLines) const
    {
        if (text.isEmpty() || rect.width() <= 0 || rect.height() <= 0)
            return;
        const LineRanges lines = layoutText(text, font, rect.width(), maxLines);
        const QFontMetrics metrics(font);
        const int lineHeight = metrics.lineSpacing();
        painter->save();
        painter->setFont(font);
        painter->setPen(colour);
        int y = rect.top();
        for (int i = 0; i < lines.ranges.size(); ++i) {
            QString line = text.mid(lines.ranges.at(i).first, lines.ranges.at(i).second);
            if (lines.more && i == lines.ranges.size() - 1)
                line = metrics.elidedText(line, Qt::ElideRight, rect.width());
            painter->drawText(QRect(rect.left(), y, rect.width(), lineHeight),
                              Qt::AlignLeft | Qt::AlignTop, line);
            y += lineHeight;
        }
        painter->restore();
    }

    TrajectoryPage *m_page;
};

TrajectoryPage::TrajectoryPage(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("trajectoryPage"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setFixedHeight(theme->headerHeight());
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(theme->space5(), 0, theme->space5(), 0);
    headerLayout->setSpacing(theme->space5());

    m_title = new QLabel(QStringLiteral("Trajectory"), m_header);
    m_title->setFont(Chrome::uiFont(theme->fontProse(), true));
    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::WindowText, theme->fg());
    m_title->setPalette(titlePalette);
    headerLayout->addWidget(m_title);

    m_count = new QLabel(m_header);
    m_count->setFont(Chrome::monoFont(theme->fontData()));
    QPalette countPalette = m_count->palette();
    countPalette.setColor(QPalette::WindowText, theme->dim());
    m_count->setPalette(countPalette);
    headerLayout->addWidget(m_count);

    headerLayout->addStretch(1);

    m_lane = new ChipButton(m_header);
    m_lane->setObjectName(QStringLiteral("trajectoryLaneFilter"));
    m_lane->setCaret(true);
    m_lane->setText(laneName(-1));
    m_lane->setMinimumWidth(8 * theme->space6() + theme->space2());
    m_lane->setToolTip(QStringLiteral("Lane filter"));
    connect(m_lane, &QToolButton::clicked, this, &TrajectoryPage::openLaneMenu);
    headerLayout->addWidget(m_lane);

    m_filter = new FilterField(m_header);
    m_filter->setObjectName(QStringLiteral("trajectoryFilter"));
    m_filter->setPlaceholderText(QStringLiteral("Filter kind or text\u2026"));
    m_filter->setMinimumWidth(7 * theme->space6() + theme->space4());
    m_filter->setMaximumWidth(17 * theme->space6() + theme->space4());
    connect(m_filter, &QLineEdit::textChanged, this, &TrajectoryPage::setNeedle);
    headerLayout->addWidget(m_filter, 1);

    m_fold = new ChipButton(m_header);
    m_fold->setObjectName(QStringLiteral("trajectoryFoldAll"));
    m_fold->setText(QStringLiteral("Fold turns"));
    connect(m_fold, &QToolButton::clicked, this, &TrajectoryPage::toggleAllFolds);
    headerLayout->addWidget(m_fold);

    layout->addWidget(m_header);

    m_strip = new LaneStrip(this);
    layout->addWidget(m_strip);

    m_list = new List(this);
    m_list->setObjectName(QStringLiteral("trajectoryList"));
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setUniformItemSizes(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::StrongFocus);
    m_list->setSpacing(0);
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    m_list->setAccessibleName(QStringLiteral("Trajectory"));
    QPalette listPalette = m_list->palette();
    listPalette.setColor(QPalette::Base, theme->bg());
    listPalette.setColor(QPalette::Window, theme->bg());
    m_list->setPalette(listPalette);
    m_list->viewport()->setPalette(listPalette);
    Chrome::styleScrollBar(m_list->verticalScrollBar());

    m_rows = new Rows(this);
    m_list->setModel(m_rows);
    m_delegate = new Delegate(this, m_list);
    m_list->setItemDelegate(m_delegate);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &TrajectoryPage::onScrolled);
    m_list->viewport()->installEventFilter(this);
    layout->addWidget(m_list, 1);

    m_empty = new QLabel(m_list->viewport());
    m_empty->setObjectName(QStringLiteral("trajectoryEmpty"));
    m_empty->setFont(Chrome::uiFont(theme->fontUI()));
    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_empty->setWordWrap(true);
    m_empty->setTextInteractionFlags(Qt::NoTextInteraction);
    QPalette emptyPalette = m_empty->palette();
    emptyPalette.setColor(QPalette::WindowText, theme->dim());
    m_empty->setPalette(emptyPalette);
    m_empty->setVisible(false);

    if (m_facade != nullptr) {
        connect(m_facade, &ClientFacade::trajectoryChanged, this,
                &TrajectoryPage::onFacadeChanged);
        connect(m_facade, &ClientFacade::busyChanged, this, &TrajectoryPage::onFacadeChanged);
        connect(m_facade, &ClientFacade::sessionIdChanged, this,
                &TrajectoryPage::onFacadeChanged);
    }

    updateFoldButton();
}

bool TrajectoryPage::supported() const
{
    return m_facade != nullptr && m_facade->hasTrajectory();
}

int TrajectoryPage::rowCount() const
{
    return m_rows != nullptr ? m_rows->rowCount() : 0;
}

void TrajectoryPage::setShown(bool shown)
{
    setVisible(shown);
}

void TrajectoryPage::setSession(const QString &sessionId)
{
    if (sessionId == m_sessionId)
        return;
    m_sessionId = sessionId;
    m_collapsedTurns.clear();
    m_expandedCells.clear();
    m_hoveredRow = -1;
    m_follow = true;
    updateFoldButton();
    if (m_active)
        refresh();
}

void TrajectoryPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_active = true;
    refresh();
}

void TrajectoryPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_active = false;
}

void TrajectoryPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionEmptyState();
}

bool TrajectoryPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list->viewport() && event->type() == QEvent::Leave) {
        if (m_hoveredRow != -1) {
            m_hoveredRow = -1;
            m_list->viewport()->update();
        }
        m_list->viewport()->setCursor(Qt::ArrowCursor);
    }
    return QWidget::eventFilter(watched, event);
}

void TrajectoryPage::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.fillRect(QRect(0, 0, width(), theme->headerHeight()), theme->panel());
    painter.fillRect(QRect(0, theme->headerHeight() - theme->hairline(), width(),
                           theme->hairline()),
                     theme->border());
}

void TrajectoryPage::onFacadeChanged()
{
    if (!m_active)
        return;
    refresh();
}

void TrajectoryPage::refresh()
{
    if (!m_active)
        return;
    m_cells = m_facade != nullptr ? m_facade->trajectory() : QVariantList();
    const int total = int(m_cells.size());
    m_count->setText(QStringLiteral("%1 record%2")
                         .arg(total)
                         .arg(total == 1 ? QString() : QStringLiteral("s")));
    rebuildRows();
}

void TrajectoryPage::rebuildRows()
{
    if (!m_active)
        return;
    if (m_rows == nullptr)
        return;

    QScrollBar *bar = m_list->verticalScrollBar();
    const QModelIndex topIndex = m_list->indexAt(QPoint(0, 0));
    const QString anchorKey = topIndex.isValid() ? m_rows->keyAt(topIndex.row()) : QString();
    const int anchorOffset = topIndex.isValid() ? -m_list->visualRect(topIndex).top() : 0;
    const bool follow = m_follow;

    const QVariantList cells = filteredCells();
    QVector<QString> order;
    QHash<QString, QVector<QVariantMap>> sections;
    QHash<QString, int> turns;
    int maxTurn = 0;
    for (const QVariant &value : cells) {
        const QVariantMap cell = value.toMap();
        const int turn = cell.value(QStringLiteral("turn")).toInt();
        const QString key = QString::number(turn);
        if (!sections.contains(key)) {
            order.append(key);
            turns.insert(key, turn);
        }
        sections[key].append(cell);
        maxTurn = qMax(maxTurn, turn);
    }

    QVector<Row> rows;
    for (const QString &key : order) {
        const QVector<QVariantMap> records = sections.value(key);
        const int turn = turns.value(key);

        QSet<int> steps;
        int tools = 0;
        for (const QVariantMap &cell : records) {
            const int step = cell.value(QStringLiteral("step")).toInt();
            if (step > 0)
                steps.insert(step);
            const QString kind = cell.value(QStringLiteral("kind")).toString();
            if (kind == QLatin1String("tool") || kind == QLatin1String("subtool"))
                ++tools;
        }

        Row turnRow;
        turnRow.type = Row::Turn;
        turnRow.turn = turn;
        turnRow.open = m_facade != nullptr && m_facade->busy() && turn == maxTurn;
        turnRow.key = QStringLiteral("t") + key;
        rows.append(turnRow);

        if (isTurnCollapsed(turn)) {
            Row collapsed;
            collapsed.type = Row::Collapsed;
            collapsed.turn = turn;
            collapsed.stepCount = int(steps.size());
            collapsed.toolCount = tools;
            collapsed.key = QStringLiteral("x") + key;
            rows.append(collapsed);
            continue;
        }

        int lastStep = -1;
        for (const QVariantMap &cell : records) {
            const int step = cell.value(QStringLiteral("step")).toInt();
            if (step != lastStep) {
                if (step > 0) {
                    Row stepRow;
                    stepRow.type = Row::Step;
                    stepRow.turn = turn;
                    stepRow.step = step;
                    stepRow.key = QStringLiteral("s") + key + QLatin1Char('.')
                                  + QString::number(step);
                    rows.append(stepRow);
                }
                lastStep = step;
            }
            Row cellRow;
            cellRow.type = Row::Cell;
            cellRow.turn = turn;
            cellRow.step = step;
            cellRow.cell = cell;
            cellRow.key = QStringLiteral("c") + cell.value(QStringLiteral("id")).toString();
            rows.append(cellRow);
        }
    }

    m_relayouting = true;
    m_rows->reload(std::move(rows));
    updateFoldButton();
    positionEmptyState();
    if (follow) {
        m_list->scrollToBottom();
    } else if (!anchorKey.isEmpty()) {
        const int anchor = m_rows->indexOfKey(anchorKey);
        if (anchor >= 0) {
            m_list->scrollTo(m_rows->index(anchor, 0), QAbstractItemView::PositionAtTop);
            bar->setValue(bar->value() + anchorOffset);
        }
    }
    m_relayouting = false;
    m_strip->update();
}

void TrajectoryPage::updateFoldButton()
{
    if (m_fold == nullptr)
        return;
    m_fold->setText(m_collapsedTurns.isEmpty() ? QStringLiteral("Fold turns")
                                               : QStringLiteral("Unfold turns"));
    m_fold->updateGeometry();
}

void TrajectoryPage::positionEmptyState()
{
    if (m_empty == nullptr || m_list == nullptr)
        return;
    const bool empty = m_rows == nullptr || m_rows->rowCount() == 0;
    if (!empty) {
        m_empty->setVisible(false);
        return;
    }
    Theme *theme = Theme::instance();
    const QRect viewport = m_list->viewport()->rect();
    const int margin = 3 * theme->space5();
    m_empty->setText(m_cells.isEmpty()
                         ? QStringLiteral("No trajectory records yet.\n\nMessages, context, "
                                          "compactions and tool calls appear here as the model's "
                                          "input/output ledger once this session has history.")
                         : QStringLiteral("No record matches the filter."));
    m_empty->setGeometry(viewport.left() + margin, viewport.top() + margin,
                         qMax(0, viewport.width() - 2 * margin),
                         qMax(0, viewport.height() - 2 * margin));
    m_empty->setVisible(true);
    m_empty->raise();
}

void TrajectoryPage::onScrolled()
{
    if (m_relayouting)
        return;
    QScrollBar *bar = m_list->verticalScrollBar();
    m_follow = bar->value() >= bar->maximum();
}

void TrajectoryPage::activateRow(int index, int offset)
{
    const Row *row = m_rows != nullptr ? m_rows->rowAt(index) : nullptr;
    if (row == nullptr || !rowBodyContains(index, offset))
        return;
    if (row->type == Row::Turn) {
        toggleTurn(row->turn);
        return;
    }
    if (row->type == Row::Cell && cellExpandable(row->cell))
        toggleCell(row->cell.value(QStringLiteral("id")).toString());
}

bool TrajectoryPage::rowBodyContains(int index, int offset) const
{
    const Row *row = m_rows != nullptr ? m_rows->rowAt(index) : nullptr;
    if (row == nullptr)
        return false;
    if (row->type == Row::Turn)
        return true;
    if (row->type != Row::Cell)
        return false;
    const int top = Theme::instance()->space3();
    const int height = bodyHeight(row->cell, m_list->viewport()->width());
    return offset >= top && offset < top + height;
}

void TrajectoryPage::toggleTurn(int turn)
{
    if (m_collapsedTurns.contains(turn))
        m_collapsedTurns.remove(turn);
    else
        m_collapsedTurns.insert(turn, true);
    rebuildRows();
}

void TrajectoryPage::toggleCell(const QString &id)
{
    if (id.isEmpty())
        return;
    if (m_expandedCells.contains(id))
        m_expandedCells.remove(id);
    else
        m_expandedCells.insert(id);
    rebuildRows();
}

void TrajectoryPage::toggleAllFolds()
{
    if (!m_collapsedTurns.isEmpty()) {
        m_collapsedTurns.clear();
    } else {
        for (const QVariant &value : m_cells) {
            const QVariantMap cell = value.toMap();
            m_collapsedTurns.insert(cell.value(QStringLiteral("turn")).toInt(), true);
        }
    }
    rebuildRows();
}

void TrajectoryPage::setLaneFilter(int lane)
{
    if (lane == m_laneFilter)
        return;
    m_laneFilter = lane;
    if (m_lane != nullptr) {
        m_lane->setText(laneName(lane));
        m_lane->updateGeometry();
    }
    rebuildRows();
}

void TrajectoryPage::setNeedle(const QString &needle)
{
    if (needle == m_needle)
        return;
    m_needle = needle;
    rebuildRows();
}

void TrajectoryPage::openLaneMenu()
{
    QMenu *menu = Chrome::shellMenu(m_lane);
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    for (int lane = -1; lane <= 2; ++lane) {
        QAction *action = menu->addAction(laneName(lane));
        action->setData(lane);
        action->setCheckable(true);
        action->setChecked(lane == m_laneFilter);
        group->addAction(action);
    }
    connect(menu, &QMenu::triggered, this,
            [this](QAction *action) { setLaneFilter(action->data().toInt()); });
    Chrome::deleteOnHide(menu);
    const QRect anchor(m_lane->mapToGlobal(QPoint(0, m_lane->height())), m_lane->size());
    Chrome::popupMenuAt(anchor, menu, false);
}

void TrajectoryPage::jumpToCell(const QString &id)
{
    if (id.isEmpty() || m_rows == nullptr)
        return;
    const int row = m_rows->indexOfKey(QStringLiteral("c") + id);
    if (row < 0)
        return;
    m_relayouting = true;
    m_list->scrollTo(m_rows->index(row, 0), QAbstractItemView::PositionAtTop);
    m_relayouting = false;
    onScrolled();
}

bool TrajectoryPage::isTurnCollapsed(int turn) const
{
    return m_collapsedTurns.value(turn, false);
}

bool TrajectoryPage::isCellExpanded(const QString &id) const
{
    return m_expandedCells.contains(id);
}

bool TrajectoryPage::cellExpandable(const QVariantMap &cell) const
{
    return !cell.value(QStringLiteral("input")).toString().isEmpty()
           || !cell.value(QStringLiteral("output")).toString().isEmpty()
           || !cell.value(QStringLiteral("thinking")).toString().isEmpty();
}

QVariantList TrajectoryPage::filteredCells() const
{
    const QString needle = m_needle.trimmed().toLower();
    QVariantList out;
    out.reserve(m_cells.size());
    for (const QVariant &value : m_cells) {
        const QVariantMap cell = value.toMap();
        if (m_laneFilter >= 0 && cell.value(QStringLiteral("lane")).toInt() != m_laneFilter)
            continue;
        if (!needle.isEmpty()) {
            const QString hay =
                (cell.value(QStringLiteral("kind")).toString() + QLatin1Char(' ')
                 + cell.value(QStringLiteral("text")).toString() + QLatin1Char(' ')
                 + cell.value(QStringLiteral("summary")).toString() + QLatin1Char(' ')
                 + cell.value(QStringLiteral("toolName")).toString() + QLatin1Char(' ')
                 + cell.value(QStringLiteral("status")).toString())
                    .toLower();
            if (!hay.contains(needle))
                continue;
        }
        out.append(cell);
    }
    return out;
}

QVector<int> TrajectoryPage::laneCounts() const
{
    QVector<int> counts(3, 0);
    for (const QVariant &value : m_cells) {
        const int lane = value.toMap().value(QStringLiteral("lane")).toInt();
        if (lane >= 0 && lane < 3)
            counts[lane] += 1;
    }
    return counts;
}

QString TrajectoryPage::summaryText(const QVariantMap &cell) const
{
    const QString text = cell.value(QStringLiteral("text")).toString();
    const QString summary = cell.value(QStringLiteral("summary")).toString();
    if (!text.isEmpty() && !summary.isEmpty())
        return text + QStringLiteral(" \u00b7 ") + summary;
    return !text.isEmpty() ? text : summary;
}

QString TrajectoryPage::resultText(const QVariantMap &cell) const
{
    const QString result = cell.value(QStringLiteral("result")).toString();
    const QString preview = cell.value(QStringLiteral("resultPreview")).toString();
    if (!result.isEmpty() && !preview.isEmpty())
        return QStringLiteral("\u2192 ") + result + QStringLiteral(" \u00b7 ") + preview;
    if (!result.isEmpty())
        return QStringLiteral("\u2192 ") + result;
    if (!preview.isEmpty())
        return QStringLiteral("\u2192 ") + preview;
    return QString();
}

QColor TrajectoryPage::laneColour(int lane) const
{
    Theme *theme = Theme::instance();
    switch (lane) {
    case 0:
        return theme->dim();
    case 1:
        return theme->fg();
    case 2:
        return theme->faint();
    default:
        return theme->faint();
    }
}

bool TrajectoryPage::rowActivatable(int index) const
{
    const Row *row = m_rows != nullptr ? m_rows->rowAt(index) : nullptr;
    if (row == nullptr)
        return false;
    if (row->type == Row::Turn)
        return true;
    return row->type == Row::Cell && cellExpandable(row->cell);
}

int TrajectoryPage::lanePitch() const
{
    Theme *theme = Theme::instance();
    return theme->fontData() + 2 * theme->space1() + theme->hairline();
}

int TrajectoryPage::captionWidth() const
{
    Theme *theme = Theme::instance();
    return 5 * theme->space6() - theme->hairline();
}

int TrajectoryPage::kindSlot() const
{
    Theme *theme = Theme::instance();
    return 4 * theme->space6() + 2 * theme->space3();
}

int TrajectoryPage::seqColumn() const
{
    Theme *theme = Theme::instance();
    return 2 * theme->space6() + theme->space4();
}

int TrajectoryPage::contentLeft() const
{
    Theme *theme = Theme::instance();
    return theme->space5() + kindSlot() + theme->space3() + seqColumn() + theme->space3();
}

int TrajectoryPage::stripHeight() const
{
    Theme *theme = Theme::instance();
    return 3 * lanePitch() + 2 * theme->space3();
}

int TrajectoryPage::contentRight(const QVariantMap &cell, int width) const
{
    Theme *theme = Theme::instance();
    const QFontMetrics metrics(Chrome::monoFont(theme->fontData()));
    const int statusWidth =
        metrics.horizontalAdvance(cell.value(QStringLiteral("status")).toString());
    const int right = width - 2 * theme->space5() - theme->space3() - statusWidth
                      - theme->space4();
    return qMax(contentLeft() + theme->space6(), right);
}

int TrajectoryPage::maxTextLines(bool expanded) const
{
    Theme *theme = Theme::instance();
    return expanded ? 4 * theme->space6() : theme->space1();
}

int TrajectoryPage::bodyHeight(const QVariantMap &cell, int width) const
{
    Theme *theme = Theme::instance();
    const QString kind = cell.value(QStringLiteral("kind")).toString();
    const bool machine = kind == QLatin1String("tool") || kind == QLatin1String("subtool");
    const bool expanded = isCellExpanded(cell.value(QStringLiteral("id")).toString());
    const int lines = maxTextLines(expanded);
    const int textWidth = qMax(0, contentRight(cell, width) - contentLeft());

    int height = 0;
    const QString summary = summaryText(cell);
    if (!summary.isEmpty()) {
        const QFont font = machine ? Chrome::monoFont(theme->fontMono())
                                   : Chrome::uiFont(theme->fontProse());
        height += wrappedHeight(summary, font, textWidth, lines);
    }
    const QString result = resultText(cell);
    if (!result.isEmpty()) {
        if (height > 0)
            height += theme->space1();
        height += wrappedHeight(result, Chrome::monoFont(theme->fontData()), textWidth, lines);
    }
    return qMax(height, theme->fontProse());
}

int TrajectoryPage::detailHeight(const QVariantMap &cell, int width) const
{
    Theme *theme = Theme::instance();
    QFont labelFont = Chrome::monoFont(theme->fontData());
    labelFont.setLetterSpacing(QFont::AbsoluteSpacing, theme->space1() / 4.0);
    const QFont bodyFont = Chrome::monoFont(theme->fontMono());
    const int labelHeight = QFontMetrics(labelFont).height();
    const int textWidth =
        qMax(0, width - contentLeft() - theme->space5() - 2 * theme->space5());

    int height = 0;
    int blocks = 0;
    const QVector<QString> bodies{
        cell.value(QStringLiteral("thinking")).toString(),
        cell.value(QStringLiteral("input")).toString(),
        cell.value(QStringLiteral("output")).toString(),
    };
    for (const QString &body : bodies) {
        if (body.isEmpty())
            continue;
        if (blocks > 0)
            height += theme->space4();
        height += labelHeight + theme->space2() + wrappedHeight(body, bodyFont, textWidth, 0)
                  + 2 * theme->space4();
        ++blocks;
    }
    return height;
}

int TrajectoryPage::rowHeight(int index, int width) const
{
    Theme *theme = Theme::instance();
    const Row *row = m_rows != nullptr ? m_rows->rowAt(index) : nullptr;
    if (row == nullptr)
        return theme->rowHeight();
    switch (row->type) {
    case Row::Turn:
        return theme->rowHeight();
    case Row::Step:
    case Row::Collapsed:
        return theme->space6() + theme->space3() + theme->hairline();
    case Row::Cell: {
        const bool expanded = isCellExpanded(row->cell.value(QStringLiteral("id")).toString());
        return 2 * theme->space3() + bodyHeight(row->cell, width)
               + (expanded ? detailHeight(row->cell, width) : 0);
    }
    }
    return theme->rowHeight();
}

int TrajectoryPage::wrappedHeight(const QString &text, const QFont &font, int width,
                                  int maxLines) const
{
    return wrappedTextHeight(text, font, width, maxLines);
}
