#include "FilesDock.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>
#include <QWheelEvent>

#include <functional>

namespace {

QString grouped(qint64 value)
{
    const QString digits = QString::number(value);
    QString out;
    out.reserve(digits.size() + digits.size() / 3);
    for (int index = 0; index < digits.size(); ++index) {
        if (index > 0 && (digits.size() - index) % 3 == 0)
            out += QLatin1Char(',');
        out += digits.at(index);
    }
    return out;
}


class Button : public QWidget
{
public:
    explicit Button(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_text(text)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }

    void setHandler(std::function<void()> handler) { m_handler = std::move(handler); }

    QSize sizeHint() const override
    {
        const int side = Theme::instance()->controlHeight();
        return QSize(side, side);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        Chrome::paintChip(painter, rect(), m_hovered && isEnabled(), false);
        painter.setFont(Chrome::uiFont(theme->fontUI()));
        painter.setPen(isEnabled() ? theme->fg() : theme->faint());
        painter.drawText(rect(), Qt::AlignCenter, m_text);
    }

    void enterEvent(QEnterEvent *) override
    {
        m_hovered = true;
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = false;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !isEnabled()) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (!rect().contains(event->position().toPoint())) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (m_handler)
            m_handler();
        event->accept();
    }

private:
    QString m_text;
    std::function<void()> m_handler;
    bool m_hovered = false;
};

}

class FilesDock::Header : public QWidget
{
public:
    explicit Header(QWidget *parent)
        : QWidget(parent)
    {
        Theme *theme = Theme::instance();
        setFixedHeight(theme->headerHeight());
        m_refresh = new Button(QStringLiteral("\u21bb"), this);
        m_refresh->setFixedSize(theme->controlHeight(), theme->controlHeight());
        m_refresh->setToolTip(QStringLiteral("Refresh"));
    }

    void setRefreshHandler(std::function<void()> handler) { m_refresh->setHandler(std::move(handler)); }
    void setAvailable(bool available) { m_refresh->setEnabled(available); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        const int right = m_refresh ? m_refresh->x() - theme->space3() : width();
        const QRect title(theme->space4(), 0, qMax(0, right - theme->space4()), height());
        Chrome::paintLine(painter, title, Chrome::uiFont(theme->fontProse(), true), theme->fg(),
                          QStringLiteral("Files"));
        painter.fillRect(QRect(0, height() - theme->hairline(), width(), theme->hairline()),
                         theme->border());
    }

    void resizeEvent(QResizeEvent *) override
    {
        if (!m_refresh)
            return;
        Theme *theme = Theme::instance();
        const int side = theme->controlHeight();
        m_refresh->move(width() - theme->space4() - side, (height() - side) / 2);
    }

private:
    Button *m_refresh = nullptr;
};

class FilesDock::Line : public QWidget
{
public:
    explicit Line(QWidget *parent)
        : QWidget(parent)
    {
        Theme *theme = Theme::instance();
        m_font = Chrome::monoFont(theme->fontData());
        setFixedHeight(QFontMetrics(m_font).height() + theme->space3() + theme->space4());
    }

    void setText(const QString &text, bool error)
    {
        if (text == m_text && error == m_error)
            return;
        m_text = text;
        m_error = error;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        const QRect area(theme->space4(), theme->space3(), width() - 2 * theme->space4(),
                         height() - theme->space3() - theme->space4());
        Chrome::paintLine(painter, area, m_font, m_error ? theme->danger() : theme->dim(),
                          m_text, Qt::AlignLeft | Qt::AlignVCenter, true);
    }

private:
    QFont m_font;
    QString m_text;
    bool m_error = false;
};

class FilesDock::Tree : public QWidget
{
public:
    explicit Tree(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setRows(const QVariantList &rows)
    {
        if (rows == m_rows)
            return;
        m_rows = rows;
        if (m_hovered >= int(m_rows.size()))
            m_hovered = -1;
        m_offset = qBound(0, m_offset, maximumOffset());
        update();
    }

    void setCurrentPath(const QString &path)
    {
        if (path == m_currentPath)
            return;
        m_currentPath = path;
        update();
    }

    void setSelectedPath(const QString &path)
    {
        if (path == m_selectedPath)
            return;
        m_selectedPath = path;
        update();
    }

    void setEmptyText(const QString &text, bool error)
    {
        if (text == m_emptyText && error == m_emptyError)
            return;
        m_emptyText = text;
        m_emptyError = error;
        update();
    }

    void setDirectoryHandler(std::function<void(const QString &)> handler)
    {
        m_directory = std::move(handler);
    }

    void setToggleHandler(std::function<void(const QString &)> handler)
    {
        m_toggle = std::move(handler);
    }

    void setFileHandler(std::function<void(const QString &)> handler)
    {
        m_file = std::move(handler);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.fillRect(rect(), theme->panel());
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        if (m_rows.isEmpty()) {
            painter.setFont(Chrome::uiFont(theme->fontUI()));
            painter.setPen(m_emptyError ? theme->danger() : theme->dim());
            const int inset = theme->space6();
            painter.drawText(rect().adjusted(inset, 0, -inset, 0),
                             Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap,
                             m_emptyText);
            return;
        }

        const int rowHeight = theme->rowHeight();
        const QFont dataFont = Chrome::monoFont(theme->fontData());
        const QFontMetrics dataMetrics(dataFont);
        const int caretWidth = qMax(dataMetrics.horizontalAdvance(QStringLiteral("\u25be")),
                                    dataMetrics.horizontalAdvance(QStringLiteral("\u25b8")));
        const int glyphWidth = qMax(dataMetrics.horizontalAdvance(QStringLiteral("\u25a0")),
                                    dataMetrics.horizontalAdvance(QStringLiteral("\u00b7")));
        const int accentWidth = theme->hairline() * 3;

        const int first = m_offset / rowHeight;
        const int last = qMin(int(m_rows.size()) - 1, (m_offset + height()) / rowHeight);
        for (int index = first; index <= last; ++index) {
            const QVariantMap entry = m_rows.at(index).toMap();
            const QString name = entry.value(QStringLiteral("name")).toString();
            const QString path = entry.value(QStringLiteral("path")).toString();
            const QString type = entry.value(QStringLiteral("type")).toString();
            const int depth = entry.value(QStringLiteral("depth")).toInt();
            const bool isDirectory = type == QLatin1String("directory");
            const bool expanded = entry.value(QStringLiteral("expanded")).toBool();
            const bool current = isDirectory && path == m_currentPath;
            const bool selected = !isDirectory && path == m_selectedPath;
            const int y = index * rowHeight - m_offset;
            const QRect rowRect(0, y, width(), rowHeight);

            if (selected || index == m_hovered)
                painter.fillRect(rowRect, theme->raised());
            if (selected)
                painter.fillRect(QRect(0, y, accentWidth, rowHeight), theme->accent());

            const int indent = theme->space4() + depth * theme->space6();
            if (isDirectory) {
                painter.setFont(dataFont);
                painter.setPen(theme->dim());
                painter.drawText(QRect(indent, y, caretWidth, rowHeight),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 expanded ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"));
            }

            const int glyphX = indent + caretWidth + theme->space1();
            painter.setFont(dataFont);
            painter.setPen(theme->faint());
            painter.drawText(QRect(glyphX, y, glyphWidth, rowHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             isDirectory ? QStringLiteral("\u25a0") : QStringLiteral("\u00b7"));

            const QVariant size = entry.value(QStringLiteral("size"));
            const bool hasSize = !isDirectory && size.isValid() && size.toInt() >= 0;
            const QString sizeText = hasSize ? grouped(size.toLongLong()) : QString();
            int right = width() - theme->space4();
            if (hasSize) {
                const int sizeWidth = dataMetrics.horizontalAdvance(sizeText);
                painter.setFont(dataFont);
                painter.setPen(theme->dim());
                painter.drawText(QRect(right - sizeWidth, y, sizeWidth, rowHeight),
                                 Qt::AlignRight | Qt::AlignVCenter, sizeText);
                right -= sizeWidth + theme->space3();
            }

            const int nameX = glyphX + glyphWidth + theme->space2();
            const int nameWidth = qMax(0, right - nameX);
            const QFont nameFont = Chrome::monoFont(theme->fontMono(), current);
            painter.setFont(nameFont);
            painter.setPen(theme->fg());
            const QString shown = QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameWidth);
            painter.drawText(QRect(nameX, y, nameWidth, rowHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, shown);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int index = rowAt(int(event->position().y()));
        if (index != m_hovered) {
            m_hovered = index;
            update();
        }
        setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (m_hovered >= 0) {
            m_hovered = -1;
            update();
        }
        setCursor(Qt::ArrowCursor);
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        const int index = rowAt(int(event->position().y()));
        if (index < 0) {
            QWidget::mousePressEvent(event);
            return;
        }

        const QVariantMap entry = m_rows.at(index).toMap();
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QString type = entry.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("directory")) {
            const int depth = entry.value(QStringLiteral("depth")).toInt();
            const int indent = Theme::instance()->space4() + depth * Theme::instance()->space6();
            const QFontMetrics metrics(Chrome::monoFont(Theme::instance()->fontData()));
            const int caretWidth = qMax(metrics.horizontalAdvance(QStringLiteral("\u25be")),
                                        metrics.horizontalAdvance(QStringLiteral("\u25b8")));
            const int caretRight = indent + caretWidth + Theme::instance()->space2();
            if (event->position().x() >= indent && event->position().x() <= caretRight) {
                if (m_toggle)
                    m_toggle(path);
            } else if (m_directory) {
                m_directory(path);
            }
        } else if (m_file) {
            m_file(path);
        }

        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (m_rows.isEmpty()) {
            event->ignore();
            return;
        }
        const int step = event->angleDelta().y() / 2;
        m_offset = qBound(0, m_offset - step, maximumOffset());
        update();
        event->accept();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        m_offset = qBound(0, m_offset, maximumOffset());
        QWidget::resizeEvent(event);
    }

private:
    int maximumOffset() const
    {
        return qMax(0, int(m_rows.size()) * Theme::instance()->rowHeight() - height());
    }

    int rowAt(int y) const
    {
        const int contentY = y + m_offset;
        if (contentY < 0)
            return -1;
        const int index = contentY / Theme::instance()->rowHeight();
        return index >= 0 && index < int(m_rows.size()) ? index : -1;
    }

    QVariantList m_rows;
    QString m_currentPath;
    QString m_selectedPath;
    QString m_emptyText;
    std::function<void(const QString &)> m_directory;
    std::function<void(const QString &)> m_toggle;
    std::function<void(const QString &)> m_file;
    bool m_emptyError = false;
    int m_hovered = -1;
    int m_offset = 0;
};

FilesDock::FilesDock(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("workspaceFilesDock"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->hairline(), 0, 0, 0);
    layout->setSpacing(0);

    m_header = new Header(this);
    m_header->setRefreshHandler([this]() {
        if (available() && m_facade)
            m_facade->listDirectory(m_facade->filePath());
    });
    layout->addWidget(m_header);

    m_breadcrumb = new QWidget(this);
    m_breadcrumb->setFixedHeight(theme->controlHeight() + 2 * theme->space4());
    auto *breadcrumbLayout = new QHBoxLayout(m_breadcrumb);
    breadcrumbLayout->setContentsMargins(theme->space4(), theme->space4(), theme->space4(),
                                         theme->space4());
    breadcrumbLayout->setSpacing(theme->space2());

    auto *up = new Button(QStringLiteral("\u2191"), m_breadcrumb);
    up->setFixedSize(theme->controlHeight(), theme->controlHeight());
    up->setToolTip(QStringLiteral("Go up"));
    up->setHandler([this]() {
        if (available() && m_facade)
            m_facade->goUpDirectory();
    });
    m_up = up;
    breadcrumbLayout->addWidget(up);

    m_path = new QLineEdit(m_breadcrumb);
    m_path->setReadOnly(true);
    m_path->setFrame(false);
    m_path->setFont(Chrome::monoFont(theme->fontData()));
    m_path->setMinimumWidth(0);
    m_path->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2;"
                                         " border: %3px solid %4; border-radius: %5px;"
                                         " padding: 0 %6px; }")
                              .arg(theme->fieldFill().name(), theme->fg().name())
                              .arg(theme->hairline())
                              .arg(theme->border().name())
                              .arg(theme->radius())
                              .arg(theme->space2()));
    QPalette pathPalette = m_path->palette();
    pathPalette.setColor(QPalette::Base, theme->fieldFill());
    pathPalette.setColor(QPalette::Text, theme->fg());
    m_path->setPalette(pathPalette);
    connect(m_path, &QLineEdit::textChanged, this,
            [this]() { m_path->setCursorPosition(m_path->text().size()); });
    breadcrumbLayout->addWidget(m_path, 1);

    layout->addWidget(m_breadcrumb);

    m_tree = new Tree(this);
    m_tree->setDirectoryHandler([this](const QString &path) {
        if (available() && m_facade)
            m_facade->listDirectory(path);
    });
    m_tree->setToggleHandler([this](const QString &path) {
        if (available() && m_facade)
            m_facade->toggleDirectory(path);
    });
    m_tree->setFileHandler([this](const QString &path) {
        if (available() && m_facade)
            m_facade->readFile(path);
    });
    layout->addWidget(m_tree, 1);

    auto *rule = new QWidget(this);
    rule->setFixedHeight(theme->hairline());
    rule->setAutoFillBackground(true);
    QPalette rulePalette = rule->palette();
    rulePalette.setColor(QPalette::Window, theme->border());
    rule->setPalette(rulePalette);
    layout->addWidget(rule);

    m_viewer = new QPlainTextEdit(this);
    m_viewer->setReadOnly(true);
    m_viewer->setFrameShape(QFrame::NoFrame);
    m_viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_viewer->setFont(Chrome::monoFont(theme->fontMono()));
    m_viewer->document()->setDocumentMargin(theme->space4());
    QPalette viewerPalette = m_viewer->palette();
    viewerPalette.setColor(QPalette::Base, theme->raised());
    viewerPalette.setColor(QPalette::Text, theme->fg());
    viewerPalette.setColor(QPalette::PlaceholderText, theme->caption());
    m_viewer->setPalette(viewerPalette);
    Chrome::styleScrollBar(m_viewer->verticalScrollBar());
    Chrome::styleScrollBar(m_viewer->horizontalScrollBar());
    layout->addWidget(m_viewer, 1);

    m_footer = new Line(this);
    layout->addWidget(m_footer);

    if (m_facade) {
        connect(m_facade, &ClientFacade::fileEntriesChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::filePathChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::fileTextChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::fileSelectedPathChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::fileBytesChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::fileTruncatedChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::fileErrorChanged, this, &FilesDock::refresh);
        connect(m_facade, &ClientFacade::sessionIdChanged, this, &FilesDock::loadRootIfNeeded);
    }

    applyWidth();
    m_header->setAvailable(available());
    refresh();
}

FilesDock::~FilesDock() = default;

int FilesDock::dockWidth() const
{
    return (m_shown && available()) ? m_expandedWidth : 0;
}

bool FilesDock::available() const
{
    return m_facade != nullptr && m_facade->hasFileSurface();
}

void FilesDock::setShown(bool shown)
{
    if (!available())
        shown = false;
    if (shown == m_shown) {
        m_header->setAvailable(available());
        return;
    }

    m_shown = shown;
    applyWidth();
    emit shownChanged();
    emit dockWidthChanged();

    if (m_shown)
        loadRootIfNeeded();
}

void FilesDock::setExpandedWidth(int width)
{
    const int clamped = qMax(1, width);
    if (clamped == m_expandedWidth)
        return;
    m_expandedWidth = clamped;
    applyWidth();
    emit dockWidthChanged();
}

void FilesDock::applyWidth()
{
    const int width = dockWidth();
    setFixedWidth(width);
    setVisible(width > 0);
}

void FilesDock::refresh()
{
    if (!m_facade || !m_header)
        return;

    const QString path = m_facade->filePath();
    const QString selected = m_facade->fileSelectedPath();
    const QString error = m_facade->fileError();
    const QString display = path.isEmpty() ? QStringLiteral("/") : path;

    m_header->setAvailable(available());
    if (m_path->text() != display)
        m_path->setText(display);
    m_up->setEnabled(available() && !path.isEmpty());

    m_tree->setCurrentPath(path);
    m_tree->setSelectedPath(selected);
    m_tree->setEmptyText(!error.isEmpty() ? error
                                          : path.isEmpty() ? QStringLiteral("loading…")
                                                           : QStringLiteral("empty directory"),
                         !error.isEmpty());
    m_tree->setRows(m_facade->fileEntries());

    refreshViewer();
    refreshFooter();
}

void FilesDock::refreshViewer()
{
    if (!m_facade || !m_viewer)
        return;

    const QString text = m_facade->fileText();
    if (m_viewer->toPlainText() != text)
        m_viewer->setPlainText(text);

    const bool selected = !m_facade->fileSelectedPath().isEmpty();
    m_viewer->setPlaceholderText(selected ? QStringLiteral("reading…")
                                          : QStringLiteral("select a file to read its contents"));
}

void FilesDock::refreshFooter()
{
    if (!m_facade || !m_footer)
        return;

    const QString error = m_facade->fileError();
    if (!error.isEmpty()) {
        m_footer->setText(error, true);
        return;
    }

    const QString path = m_facade->filePath();
    const QString display = path.isEmpty() ? QStringLiteral("/") : path;
    const QString selected = m_facade->fileSelectedPath();
    if (selected.isEmpty()) {
        m_footer->setText(QStringLiteral("browsing ") + display, false);
        return;
    }

    const QString text = m_facade->fileText();
    const int lines = text.isEmpty() ? 0 : int(text.count(QLatin1Char('\n'))) + 1;
    QString line = selected + QStringLiteral("  ") + grouped(m_facade->fileBytes())
                   + QStringLiteral(" bytes");
    if (m_facade->fileTruncated())
        line += QStringLiteral("  (first %1 lines shown)").arg(lines);
    m_footer->setText(line, false);
}

void FilesDock::loadRootIfNeeded()
{
    if (!available() || !m_shown || !m_facade)
        return;
    if (m_facade->fileEntries().isEmpty() && m_facade->fileError().isEmpty()
        && m_facade->filePath().isEmpty())
        m_facade->listDirectory(QString());
}

void FilesDock::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);
    painter.fillRect(rect(), theme->panel());
    painter.fillRect(QRect(0, 0, theme->hairline(), height()), theme->border());
}
