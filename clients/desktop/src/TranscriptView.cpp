#include "TranscriptView.h"

#include "AcpClient.h"
#include "ClientFacade.h"
#include "Markdown.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAbstractItemModel>
#include <QAbstractAnimation>
#include <QAbstractScrollArea>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>

namespace {

constexpr int MarkdownRhythmPercent = 149;

QString markdownRhythm()
{
    return QString::number(MarkdownRhythmPercent) + QLatin1Char('%');
}

struct RowData
{
    QString kind;
    QString text;
    QString title;
    QString status;
    QString detail;
    QString request;
    QString fullText;
    QString key;
    qint64 time = 0;
    int turn = -1;
    bool turnLead = false;
    int index = 0;
    int count = 0;
    bool expanded = false;
    bool closesTurn = true;
};

enum class RowClass {
    Placeholder,
    User,
    Agent,
    Notice,
    Error,
    Compacted,
    Tool,
    Thought,
    Image,
    File,
    MessageFile,
    PendingSend
};

RowClass rowClassFor(const RowData &data)
{
    if (data.kind == QLatin1String("user")) {
        if (data.status == QLatin1String("queued")
                || data.status == QLatin1String("steering"))
            return RowClass::PendingSend;
        return RowClass::User;
    }
    if (data.kind == QLatin1String("agent"))
        return RowClass::Agent;
    if (data.kind == QLatin1String("tool"))
        return RowClass::Tool;
    if (data.kind == QLatin1String("thought"))
        return RowClass::Thought;
    if (data.kind == QLatin1String("notice"))
        return RowClass::Notice;
    if (data.kind == QLatin1String("error"))
        return RowClass::Error;
    if (data.kind == QLatin1String("compacted"))
        return RowClass::Compacted;
    if (data.kind == QLatin1String("image"))
        return RowClass::Image;
    if (data.kind == QLatin1String("file"))
        return data.status == QLatin1String("attached") ? RowClass::MessageFile
                                                        : RowClass::File;
    return RowClass::Placeholder;
}

QString rowIdentity(const RowData &data)
{
    return data.key.isEmpty() ? QStringLiteral("i:%1").arg(data.index)
                              : QStringLiteral("k:%1").arg(data.key);
}

constexpr int BuildSliceBudgetMs = 8;


bool looksInjected(const QString &text)
{
    if (text.startsWith(QLatin1String("<system-reminder>")))
        return true;
    return text.contains(QLatin1String("This complete catalog replaces"))
        || text.contains(QLatin1String("The following workspace instructions may be relevant"));
}

RowData rowDataFor(const QAbstractItemModel *model, const QSet<QString> &expandedRows, int index)
{
    RowData data;
    const QModelIndex mi = model->index(index, 0);
    data.index = index;
    data.count = model->rowCount();
    data.kind = model->data(mi, AcpTranscript::KindRole).toString();
    data.text = model->data(mi, AcpTranscript::TextRole).toString();
    data.title = model->data(mi, AcpTranscript::TitleRole).toString();
    data.status = model->data(mi, AcpTranscript::StatusRole).toString();
    data.detail = model->data(mi, AcpTranscript::DetailRole).toString();
    data.request = model->data(mi, AcpTranscript::RequestRole).toString();
    data.fullText = model->data(mi, AcpTranscript::FullTextRole).toString();
    data.key = model->data(mi, AcpTranscript::KeyRole).toString();
    data.time = model->data(mi, AcpTranscript::TimeRole).toLongLong();
    data.turn = model->data(mi, AcpTranscript::TurnRole).toInt();
    data.turnLead = model->data(mi, AcpTranscript::TurnLeadRole).toBool();
    if (data.kind == QLatin1String("user") && looksInjected(data.text)) {
        data.kind = QStringLiteral("notice");
        if (data.title.isEmpty())
            data.title = QStringLiteral("agent-instructions");
    }
    data.expanded = expandedRows.contains(rowIdentity(data));
    const QString nextKind = index + 1 < model->rowCount()
        ? model->data(model->index(index + 1, 0), AcpTranscript::KindRole).toString()
        : QString();
    data.closesTurn = nextKind != QLatin1String("agent");
    return data;
}

QString rhythm(const QString &html, const QString &lineHeight)
{
    static const QRegularExpression block(QStringLiteral("<(p|li|td|th|pre)(\\s[^>]*)?>"));
    QString out;
    out.reserve(html.size() + 64);
    int last = 0;
    auto it = block.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += html.mid(last, match.capturedStart() - last);
        const QString tag = match.captured(1);
        const QString attrs = match.captured(2);
        const QString prefix = QStringLiteral("line-height:") + lineHeight + QStringLiteral("; ");
        if (attrs.contains(QStringLiteral("style=\""))) {
            QString copied = attrs;
            copied.replace(QStringLiteral("style=\""), QStringLiteral("style=\"") + prefix);
            out += QLatin1Char('<') + tag + copied + QLatin1Char('>');
        } else if (attrs.isEmpty()) {
            out += QLatin1Char('<') + tag + QStringLiteral(" style=\"") + prefix + QStringLiteral("\">");
        } else {
            out += QLatin1Char('<') + tag + attrs + QStringLiteral(" style=\"") + prefix + QStringLiteral("\">");
        }
        last = match.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

QFont uiFont(int pixelSize)
{
    QFont font(Theme::instance()->ui());
    font.setPixelSize(pixelSize);
    return font;
}

QFont monoFont(int pixelSize)
{
    QFont font(Theme::instance()->mono());
    font.setPixelSize(pixelSize);
    return font;
}

QString firstLine(const QString &text)
{
    const int cut = text.indexOf(QLatin1Char('\n'));
    QString line = cut >= 0 ? text.left(cut) : text;
    if (line.size() > 400)
        line = line.left(400);
    return line;
}

QString producerCaption(const QString &kind)
{
    if (kind == QLatin1String("agent-instructions"))
        return QStringLiteral("instructions updated");
    if (kind == QLatin1String("runtime-context"))
        return QStringLiteral("runtime context");
    if (kind == QLatin1String("time-context"))
        return QStringLiteral("time context");
    if (kind == QLatin1String("subagent-settled"))
        return QStringLiteral("subagent finished");
    if (kind == QLatin1String("tool-jobs"))
        return QStringLiteral("background job");
    if (kind == QLatin1String("goal"))
        return QStringLiteral("goal");
    return kind;
}

QString unwrapEnvelope(const QString &raw)
{
    static const QRegularExpression opening(QStringLiteral("^<([A-Za-z][\\w-]*)\\s*>"));
    QString text = raw.trimmed();
    for (int pass = 0; pass < 4; ++pass) {
        const QRegularExpressionMatch match = opening.match(text);
        if (!match.hasMatch())
            break;
        const QString closing = QStringLiteral("</") + match.captured(1) + QLatin1Char('>');
        const QString rest = text.mid(match.capturedLength());
        const int at = rest.toLower().indexOf(closing.toLower());
        text = (at >= 0 ? rest.left(at) + rest.mid(at + closing.size()) : rest).trimmed();
    }
    return text;
}

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

QPixmap markIcon(const QString &name)
{
    static QHash<QString, QPixmap> cache;
    if (name.isEmpty())
        return QPixmap();
    auto it = cache.find(name);
    if (it != cache.end())
        return it.value();
    QPixmap pixmap;
    const QString directory = iconDirectory();
    if (!directory.isEmpty()) {
        pixmap = QPixmap(directory + QLatin1Char('/') + name + QStringLiteral(".svg"));
        if (!pixmap.isNull())
            pixmap = pixmap.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    cache.insert(name, pixmap);
    return pixmap;
}

QString toolIconFor(const QString &tool)
{
    const QString name = tool.toLower();
    if (name.contains(QLatin1String("bash")) || name.contains(QLatin1String("shell"))
            || name.contains(QLatin1String("terminal")) || name.contains(QLatin1String("exec"))
            || name.contains(QLatin1String("command")))
        return QStringLiteral("shell");
    if (name.contains(QLatin1String("todo")) || name.contains(QLatin1String("goal")))
        return QStringLiteral("todo");
    if (name.contains(QLatin1String("write")) || name.contains(QLatin1String("edit"))
            || name.contains(QLatin1String("patch")) || name.contains(QLatin1String("replace"))
            || name.contains(QLatin1String("present")) || name.contains(QLatin1String("deliver")))
        return QStringLiteral("file-write");
    if (name.contains(QLatin1String("read")) || name.contains(QLatin1String("cat"))
            || name.contains(QLatin1String("file")) || name.contains(QLatin1String("list")))
        return QStringLiteral("file-read");
    if (name.contains(QLatin1String("grep")) || name.contains(QLatin1String("glob"))
            || name.contains(QLatin1String("search")) || name.contains(QLatin1String("find")))
        return QStringLiteral("search");
    if (name.contains(QLatin1String("fetch")) || name.contains(QLatin1String("web"))
            || name.contains(QLatin1String("http")))
        return QStringLiteral("web");
    if (name.contains(QLatin1String("image")) || name.contains(QLatin1String("screen"))
            || name.contains(QLatin1String("photo")))
        return QStringLiteral("image");
    if (name.contains(QLatin1String("subagent")) || name.contains(QLatin1String("task"))
            || name.contains(QLatin1String("agent")))
        return QStringLiteral("subagent");
    return QStringLiteral("tool");
}

QString toolGlyphFor(const QString &tool)
{
    const QString name = tool.toLower();
    if (name.startsWith(QLatin1String("cordis")))
        return QStringLiteral("\u25c6");
    if (name.contains(QLatin1String("bash")) || name.contains(QLatin1String("shell"))
            || name.contains(QLatin1String("terminal")) || name.contains(QLatin1String("exec"))
            || name.contains(QLatin1String("command")))
        return QStringLiteral("\u25b8_");
    if (name.contains(QLatin1String("read")) || name.contains(QLatin1String("cat"))
            || name.contains(QLatin1String("file")))
        return QStringLiteral("\u25a4");
    if (name.contains(QLatin1String("write")) || name.contains(QLatin1String("edit"))
            || name.contains(QLatin1String("patch")) || name.contains(QLatin1String("replace")))
        return QStringLiteral("\u270e");
    if (name.contains(QLatin1String("grep")) || name.contains(QLatin1String("glob"))
            || name.contains(QLatin1String("search")) || name.contains(QLatin1String("find")))
        return QStringLiteral("\u2315");
    if (name.contains(QLatin1String("fetch")) || name.contains(QLatin1String("web"))
            || name.contains(QLatin1String("http")))
        return QStringLiteral("\u21c4");
    if (name.contains(QLatin1String("image")) || name.contains(QLatin1String("screen"))
            || name.contains(QLatin1String("photo")))
        return QStringLiteral("\u25a3");
    if (name.contains(QLatin1String("subagent")) || name.contains(QLatin1String("task"))
            || name.contains(QLatin1String("agent")))
        return QStringLiteral("\u21b3");
    if (name.contains(QLatin1String("present")) || name.contains(QLatin1String("deliver")))
        return QStringLiteral("\u25a0");
    return QStringLiteral("\u25aa");
}

QString noticeIconFor(const QString &producer)
{
    if (producer == QLatin1String("subagent-settled"))
        return QStringLiteral("subagent");
    if (producer == QLatin1String("tool-jobs"))
        return QStringLiteral("job");
    if (producer == QLatin1String("goal"))
        return QStringLiteral("todo");
    return QStringLiteral("notice");
}

QString displayToolTitle(const QString &title)
{
    static const QRegularExpression single(QStringLiteral("^[a-z]+$"));
    if (single.match(title).hasMatch() && !title.isEmpty())
        return title.left(1).toUpper() + title.mid(1);
    return title;
}

QString diffMarkup(const QString &text, int lineHeightPx)
{
    static const QString prefix = QStringLiteral("line-height:%1px; color:");
    const QStringList lines = text.split(QLatin1Char('\n'));
    QString html;
    html.reserve(text.size() + lines.size() * 48);
    for (const QString &line : lines) {
        QColor colour = Theme::instance()->dim();
        if (line.startsWith(QLatin1String("+ ")))
            colour = Theme::instance()->ok();
        else if (line.startsWith(QLatin1String("- ")))
            colour = Theme::instance()->danger();
        QString escaped = line.toHtmlEscaped();
        int spaces = 0;
        while (spaces < escaped.size() && escaped.at(spaces) == QLatin1Char(' '))
            ++spaces;
        if (spaces > 0) {
            QString leading;
            for (int i = 0; i < spaces; ++i)
                leading += QStringLiteral("&nbsp;");
            escaped = leading + escaped.mid(spaces);
        }
        html += QStringLiteral("<div style=\"%1%2\">%3</div>")
                    .arg(prefix.arg(lineHeightPx), colour.name(), escaped);
    }
    return html;
}

struct FlowHeader
{
    QString icon;
    QString glyph;
    QString title;
    QColor titleColour = Qt::gray;
    int titlePixel = 13;
    bool medium = false;
    QString summary;
    QFont summaryFont;
    QColor summaryColour = Qt::gray;
    QString statusWord;
    bool chevron = false;
    bool expanded = false;
    bool rightCaret = false;
    bool canDisclose = false;
};

void paintFlowHeader(QPainter &painter, const QRect &content, const FlowHeader &head)
{
    const Theme *theme = Theme::instance();
    QFont titleFont = uiFont(head.titlePixel);
    titleFont.setWeight(head.medium ? QFont::Medium : QFont::Normal);
    const QFontMetrics titleMetrics(titleFont);
    const int afterLeading = 22;
    const int titleCap = qMax(0, qRound(content.width() * 0.55));
    const int titleInk = qMin(titleMetrics.horizontalAdvance(head.title), titleCap);
    const int dotX = content.left() + afterLeading + titleInk + theme->space4();
    const int dotSize = 2;
    const bool hasSummary = !head.summary.isEmpty();
    const int summaryX = dotX + (hasSummary ? dotSize + theme->space4() : 0);

    painter.save();

    const QRect slot(content.left(), content.top() + 4, 16, 16);
    if (head.chevron) {
        painter.setFont(uiFont(theme->fontProse()));
        painter.setPen(theme->dim());
        painter.drawText(slot, Qt::AlignCenter,
                         head.expanded ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"));
    } else if (!head.icon.isEmpty()) {
        const QPixmap mark = markIcon(head.icon);
        if (!mark.isNull()) {
            painter.drawPixmap(slot, mark);
        } else if (!head.glyph.isEmpty()) {
            painter.setFont(uiFont(theme->fontProse()));
            painter.setPen(theme->dim());
            painter.drawText(slot, Qt::AlignCenter, head.glyph);
        }
    } else if (!head.glyph.isEmpty()) {
        painter.setFont(uiFont(theme->fontProse()));
        painter.setPen(theme->dim());
        painter.drawText(slot, Qt::AlignCenter, head.glyph);
    }

    painter.setFont(titleFont);
    painter.setPen(head.titleColour);
    const QRect titleRect(content.left() + afterLeading, content.top(), titleCap,
                          content.height());
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                     titleMetrics.elidedText(head.title, Qt::ElideRight, titleCap));

    int right = content.right();
    if (!head.statusWord.isEmpty()) {
        const QFont statusFont = uiFont(theme->fontData());
        const QFontMetrics statusMetrics(statusFont);
        const int width = statusMetrics.horizontalAdvance(head.statusWord);
        painter.setFont(statusFont);
        painter.setPen(theme->danger());
        painter.drawText(QRect(right - width, content.top(), width, content.height()),
                         Qt::AlignVCenter | Qt::AlignRight, head.statusWord);
        right -= width + theme->space4();
    }

    if (head.rightCaret && head.canDisclose) {
        painter.setFont(uiFont(theme->fontData()));
        painter.setPen(theme->faint());
        painter.drawText(QRect(right - 16, content.top(), 16, content.height()),
                         Qt::AlignVCenter | Qt::AlignRight,
                         head.expanded ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"));
        right -= 16 + theme->space2();
    }

    if (hasSummary) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(head.statusWord.isEmpty() ? theme->faint() : theme->danger());
        painter.drawEllipse(QPointF(dotX, content.top() + content.height() / 2.0),
                            dotSize / 2.0, dotSize / 2.0);

        const QFontMetrics summaryMetrics(head.summaryFont);
        painter.setFont(head.summaryFont);
        painter.setPen(head.summaryColour);
        const int room = qMax(0, right - summaryX);
        painter.drawText(QRect(summaryX, content.top(), room, content.height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         summaryMetrics.elidedText(head.summary, Qt::ElideRight, room));
    }

    painter.restore();
}

}

class TranscriptRow : public QWidget
{
public:
    explicit TranscriptRow(RowClass rowClass, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_class(rowClass)
    {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setAutoFillBackground(false);
    }

    virtual void setRow(const RowData &data)
    {
        m_data = data;
        updateGeometry();
        update();
    }

    RowClass rowClass() const { return m_class; }

    const RowData &row() const { return m_data; }

    int rowIndex() const { return m_data.index; }

    virtual int bodyHeight(int width) const = 0;

    virtual bool hasContent() const { return true; }

    int topGap() const
    {
        if (m_data.index == 0)
            return 0;
        if (m_data.kind == QLatin1String("file")
                && m_data.status == QLatin1String("attached"))
            return Theme::instance()->space2();
        return Theme::instance()->space4();
    }

    QRect contentRect() const
    {
        return QRect(0, topGap(), width(), qMax(0, height() - topGap()));
    }

    int heightForWidth(int width) const override
    {
        if (!hasContent())
            return 0;
        return topGap() + bodyHeight(width);
    }

    bool hasHeightForWidth() const override { return true; }

    QSize sizeHint() const override
    {
        const int available = m_columnWidth > 0 ? m_columnWidth
                             : (width() > 0 ? width() : 600);
        return QSize(available, heightForWidth(available));
    }

    void setColumnWidth(int width)
    {
        if (m_columnWidth == width)
            return;
        m_columnWidth = width;
        updateGeometry();
    }

    QSize minimumSizeHint() const override { return QSize(0, 0); }

    std::function<void()> toggleExpanded;
    std::function<void(const QVariant &, const QVariant &, double, double)> openImage;
    std::function<void(const QString &)> openFile;
    std::function<QString(const QString &)> attachmentUrl;
    std::function<void(const QString &)> loadAttachment;
    std::function<QPixmap(const QString &)> attachmentPicture;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        layoutBody();
    }

    virtual void layoutBody() {}

    RowData m_data;

private:
    RowClass m_class;
    int m_columnWidth = 0;
};

class PlaceholderRow : public TranscriptRow
{
public:
    explicit PlaceholderRow(QWidget *parent = nullptr)
        : TranscriptRow(RowClass::Placeholder, parent)
    {
    }

    int bodyHeight(int width) const override
    {
        Q_UNUSED(width)
        return Theme::instance()->rowHeight();
    }

    bool hasContent() const override { return !m_data.kind.isEmpty(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        const QRect content = contentRect();
        if (content.isEmpty())
            return;

        QString label = m_data.title.isEmpty() ? m_data.kind : m_data.title;
        if (m_data.kind == QLatin1String("user")
                && (m_data.status == QLatin1String("queued")
                    || m_data.status == QLatin1String("steering")))
            label = m_data.status + QStringLiteral("\u2026");
        const QString preview = m_data.text.isEmpty() ? m_data.detail : m_data.text;
        QString line = label;
        if (!preview.isEmpty())
            line += QStringLiteral(" \u00b7 ") + firstLine(preview);

        QPainter painter(this);
        painter.setFont(font);
        painter.setPen(theme->dim());
        const int x = content.left() + 22;
        const int room = qMax(0, content.width() - 22);
        painter.drawText(QRect(x, content.top(), room, content.height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(line, Qt::ElideRight, room));
    }
};

class ProseEdit : public QTextEdit
{
public:
    explicit ProseEdit(QWidget *parent = nullptr)
        : QTextEdit(parent)
    {
        setObjectName(QStringLiteral("transcriptProse"));
        setReadOnly(true);
        setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAcceptDrops(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setContentsMargins(0, 0, 0, 0);
        document()->setDocumentMargin(0);
        setAutoFillBackground(false);
        viewport()->setAutoFillBackground(false);

        QPalette colours = palette();
        colours.setColor(QPalette::Base, Qt::transparent);
        colours.setColor(QPalette::Window, Qt::transparent);
        colours.setColor(QPalette::Highlight, Theme::instance()->accent());
        colours.setColor(QPalette::HighlightedText, Theme::instance()->accentFg());
        colours.setColor(QPalette::Text, Theme::instance()->fg());
        setPalette(colours);
    }

    void setMarkup(const QString &markup, const QColor &colour, int pixelSize, bool monospace)
    {
        if (m_markup == markup && m_colour == colour && m_pixelSize == pixelSize
                && m_monospace == monospace)
            return;

        m_markup = markup;
        m_colour = colour;
        m_pixelSize = pixelSize;
        m_monospace = monospace;

        const QFont font = monospace ? monoFont(pixelSize) : uiFont(pixelSize);
        setFont(font);
        QPalette colours = palette();
        colours.setColor(QPalette::Text, colour);
        setPalette(colours);
        document()->setDefaultFont(font);
        setHtml(markup);
        document()->setDocumentMargin(0);
    }

    int measure(int width) const
    {
        if (width <= 0)
            return 0;
        document()->setTextWidth(width);
        return qCeil(document()->size().height());
    }

    int naturalWidth(int cap) const
    {
        if (cap <= 0)
            return 0;
        document()->setTextWidth(cap);
        return qCeil(document()->idealWidth());
    }

private:
    QString m_markup;
    QColor m_colour;
    int m_pixelSize = 0;
    bool m_monospace = false;
};

class UserRow : public TranscriptRow
{
public:
    explicit UserRow(QWidget *parent = nullptr,
                     RowClass rowClass = RowClass::User)
        : TranscriptRow(rowClass, parent)
    {
        m_prose = new ProseEdit(this);
        m_prose->setObjectName(QStringLiteral("userProse"));
        m_prose->hide();
        setMouseTracking(true);
        m_captionHeight = QFontMetrics(uiFont(Theme::instance()->fontData())).height();
    }

    void setRow(const RowData &data) override
    {
        const QString before = messageText();
        TranscriptRow::setRow(data);
        if (!hasContent()) {
            m_prose->hide();
            updateGeometry();
            return;
        }
        if (before != messageText()) {
            const int pixelSize = Theme::instance()->fontProse() + 1;
            m_prose->setMarkup(rhythm(Markdown::instance()->toHtml(messageText()),
                                      markdownRhythm()),
                               Theme::instance()->fg(), pixelSize, false);
            m_prose->show();
        }
        updateGeometry();
        layoutBody();
    }

    int bodyHeight(int width) const override
    {
        if (!hasContent())
            return 0;
        if (pending() && messageText().isEmpty()) {
            m_bubbleHeight = 0;
            return captionBand();
        }
        measure(width);
        return captionBand() + m_bubbleHeight;
    }

    bool hasContent() const override
    {
        if (pending())
            return true;
        return !m_data.text.isEmpty()
            && m_data.status != QLatin1String("withdrawn")
            && m_data.status != QLatin1String("held");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!hasContent())
            return;
        const QRect content = contentRect();
        if (content.isEmpty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        if (pending())
            paintCaption(painter, content);
        if (m_bubbleHeight <= 0)
            return;
        painter.setBrush(Theme::instance()->bubble());
        painter.drawRoundedRect(QRectF(m_bubbleX, content.top() + captionBand(),
                                       m_bubbleWidth, m_bubbleHeight),
                                22.0, 22.0);
    }

    void layoutBody() override
    {
        if (!m_prose)
            return;
        const QRect content = contentRect();
        if (content.isEmpty() || !hasContent() || messageText().isEmpty()) {
            m_prose->hide();
            return;
        }
        measure(content.width());
        m_prose->show();
        m_prose->setGeometry(m_bubbleX + Theme::instance()->space6(),
                             content.top() + captionBand() + Theme::instance()->space5(),
                             qMax(1, m_bubbleWidth - 2 * Theme::instance()->space6()),
                             m_textHeight);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool hover = pending()
            && contentRect().contains(event->position().toPoint());
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        setCursor(hover && m_copyRect.contains(event->position().toPoint())
                      ? Qt::PointingHandCursor : Qt::ArrowCursor);
        TranscriptRow::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *) override
    {
        if (!m_hover)
            return;
        m_hover = false;
        setCursor(Qt::ArrowCursor);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (pending() && event->button() == Qt::LeftButton && m_hover
                && !messageText().isEmpty()
                && m_copyRect.contains(event->position().toPoint())) {
            QGuiApplication::clipboard()->setText(messageText());
            m_copied = true;
            update();
            QTimer::singleShot(1600, this, [this]() {
                m_copied = false;
                update();
            });
            return;
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

private:
    bool pending() const { return rowClass() == RowClass::PendingSend; }

    QString messageText() const
    {
        if (!pending())
            return m_data.text;
        return m_data.fullText.isEmpty() ? m_data.detail : m_data.fullText;
    }

    int captionBand() const
    {
        return pending() ? m_captionHeight + Theme::instance()->space2() : 0;
    }

    void paintCaption(QPainter &painter, const QRect &content)
    {
        const Theme *theme = Theme::instance();
        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        painter.setFont(font);
        painter.setPen(theme->caption());
        const QString label = m_data.status == QLatin1String("steering")
            ? QStringLiteral("steering\u2026") : QStringLiteral("queued\u2026");
        const int labelWidth = metrics.horizontalAdvance(label);
        painter.drawText(QRect(content.right() - labelWidth, content.top(),
                               labelWidth, m_captionHeight),
                         Qt::AlignVCenter | Qt::AlignRight, label);

        if (messageText().isEmpty()) {
            m_copyRect = QRect();
            return;
        }

        const int copyWidth = metrics.horizontalAdvance(QStringLiteral("Copy"));
        m_copyRect = QRect(content.right() - labelWidth - theme->space4() - copyWidth,
                           content.top(), copyWidth, m_captionHeight);
        if (!m_hover)
            return;
        painter.setPen(m_copied ? theme->ok() : theme->dim());
        painter.drawText(m_copyRect, Qt::AlignVCenter | Qt::AlignRight,
                         m_copied ? QStringLiteral("Copied") : QStringLiteral("Copy"));
    }

    void measure(int width) const
    {
        const Theme *theme = Theme::instance();
        const int column = width > 0 ? width : 600;
        const int sidePadding = theme->space6();
        const int verticalPadding = theme->space5();
        const int maxBubble = qMin(qRound(column * 0.82), 680);
        const int cap = qMax(1, maxBubble - 2 * sidePadding);
        const int natural = m_prose->naturalWidth(cap);
        int bubble = qMin(maxBubble, natural + 2 * sidePadding + 1);
        bubble = qMax(bubble, 2 * sidePadding + 1);
        m_bubbleWidth = qMax(0, qMin(bubble, column));
        m_bubbleX = column - m_bubbleWidth;
        m_textHeight = m_prose->measure(qMax(1, m_bubbleWidth - 2 * sidePadding));
        m_bubbleHeight = m_textHeight + 2 * verticalPadding;
    }

    ProseEdit *m_prose = nullptr;
    int m_captionHeight = 0;
    bool m_hover = false;
    bool m_copied = false;
    QRect m_copyRect;
    mutable int m_bubbleX = 0;
    mutable int m_bubbleWidth = 0;
    mutable int m_bubbleHeight = 0;
    mutable int m_textHeight = 0;
};

class AgentRow : public TranscriptRow
{
public:
    explicit AgentRow(QWidget *parent = nullptr)
        : TranscriptRow(RowClass::Agent, parent)
    {
        m_prose = new ProseEdit(this);
        m_prose->setObjectName(QStringLiteral("agentProse"));
        m_prose->hide();
    }

    void setRow(const RowData &data) override
    {
        const bool changed = data.text != m_data.text;
        TranscriptRow::setRow(data);
        if (!changed) {
            layoutBody();
            return;
        }
        if (m_data.text.isEmpty()) {
            m_prose->hide();
            return;
        }
        const int pixelSize = Theme::instance()->fontProse() + 1;
        m_prose->setMarkup(rhythm(Markdown::instance()->toHtml(m_data.text),
                                  markdownRhythm()),
                           Theme::instance()->fg(), pixelSize, false);
        m_prose->show();
        updateGeometry();
        layoutBody();
    }

    bool hasContent() const override
    {
        if (m_data.status == QLatin1String("withdrawn"))
            return false;
        return !m_data.text.isEmpty() || m_data.status == QLatin1String("streaming");
    }

    int bodyHeight(int width) const override
    {
        if (!hasContent())
            return 0;
        if (m_data.text.isEmpty()) {
            const QFontMetrics metrics(uiFont(Theme::instance()->fontData()));
            return metrics.height();
        }
        m_textHeight = m_prose->measure(width);
        const int footer = footerVisible() ? Theme::instance()->space2() + 19 : 0;
        return m_textHeight + footer;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QRect content = contentRect();
        if (content.isEmpty())
            return;

        QPainter painter(this);
        if (m_data.text.isEmpty()) {
            if (m_data.status != QLatin1String("streaming"))
                return;
            painter.setFont(uiFont(theme->fontData()));
            painter.setPen(theme->caption());
            painter.drawText(QRect(0, content.top(), content.width(), content.height()),
                             Qt::AlignTop | Qt::AlignLeft,
                             QStringLiteral("Deep diving"));
            return;
        }

        if (!footerVisible())
            return;

        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        painter.setFont(font);
        const QString stamp = QDateTime::fromMSecsSinceEpoch(m_data.time).toString(
            QStringLiteral("HH:mm"));
        const int y = content.top() + m_textHeight + theme->space2();
        painter.setPen(theme->caption());
        painter.drawText(QRect(0, y, metrics.horizontalAdvance(stamp), 19),
                         Qt::AlignVCenter | Qt::AlignLeft, stamp);
        const int copyX = metrics.horizontalAdvance(stamp) + theme->space4();
        m_copyRect = QRect(copyX, y, metrics.horizontalAdvance(QStringLiteral("Copy")), 19);
        painter.setPen(theme->dim());
        painter.drawText(m_copyRect, Qt::AlignVCenter | Qt::AlignLeft,
                         QStringLiteral("Copy"));
    }

    void layoutBody() override
    {
        if (!m_prose)
            return;
        const QRect content = contentRect();
        if (content.isEmpty() || m_data.text.isEmpty()) {
            m_prose->hide();
            return;
        }
        m_textHeight = m_prose->measure(content.width());
        m_prose->show();
        m_prose->setGeometry(0, content.top(), content.width(), m_textHeight);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (footerVisible() && m_copyRect.contains(event->position().toPoint())) {
            QGuiApplication::clipboard()->setText(m_data.text);
            return;
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

private:
    bool footerVisible() const
    {
        return !m_data.text.isEmpty() && m_data.closesTurn && m_data.time > 0
            && m_data.status != QLatin1String("streaming");
    }

    ProseEdit *m_prose = nullptr;
    mutable int m_textHeight = 0;
    mutable QRect m_copyRect;
};

class ExpandedRow : public TranscriptRow
{
public:
    explicit ExpandedRow(RowClass rowClass, QWidget *parent = nullptr)
        : TranscriptRow(rowClass, parent)
    {
        m_prose = new ProseEdit(this);
        m_prose->setObjectName(QStringLiteral("expandedProse"));
        m_prose->hide();
    }

    void setRow(const RowData &data) override
    {
        TranscriptRow::setRow(data);
        configureProse();
        m_prose->setVisible(payloadVisible());
        setCursor(hasPayload() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        updateGeometry();
        layoutBody();
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        const int header = headerPixels();
        if (!payloadVisible() || width <= 22)
            return header;
        m_payloadHeight = m_prose->measure(width - 22);
        return header + Theme::instance()->space2() + m_payloadHeight;
    }

    bool hasPayload() const { return !payloadText().isEmpty(); }

    bool payloadVisible() const { return m_data.expanded && hasPayload(); }

protected:
    virtual int headerPixels() const = 0;
    virtual QString payloadText() const = 0;
    virtual QString payloadMarkup() const = 0;
    virtual QColor payloadColour() const { return Theme::instance()->dim(); }
    virtual int payloadPixelSize() const { return Theme::instance()->fontProse(); }
    virtual void paintHeader(QPainter &painter, const QRect &content) = 0;

    void paintEvent(QPaintEvent *) override
    {
        const QRect content = contentRect();
        if (content.isEmpty())
            return;
        QPainter painter(this);
        paintHeader(painter, content);
    }

    void layoutBody() override
    {
        const QRect content = contentRect();
        if (content.isEmpty() || !payloadVisible()) {
            m_prose->hide();
            return;
        }
        m_payloadHeight = m_prose->measure(qMax(1, content.width() - 22));
        m_prose->show();
        m_prose->setGeometry(22, content.top() + headerPixels() + Theme::instance()->space2(),
                             qMax(1, content.width() - 22), m_payloadHeight);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (hasPayload() && toggleExpanded) {
            toggleExpanded();
            return;
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

    void configureProse()
    {
        if (!hasPayload()) {
            m_prose->hide();
            return;
        }
        m_prose->setMarkup(payloadMarkup(), payloadColour(), payloadPixelSize(), false);
    }

    ProseEdit *m_prose = nullptr;
    mutable int m_payloadHeight = 0;
};

class NoticeRow : public ExpandedRow
{
public:
    explicit NoticeRow(QWidget *parent = nullptr)
        : ExpandedRow(RowClass::Notice, parent)
    {
    }

protected:
    int headerPixels() const override { return 25; }

    QString payloadText() const override { return unwrapEnvelope(m_data.text); }

    QString payloadMarkup() const override
    {
        if (m_data.status == QLatin1String("diff"))
            return diffMarkup(payloadText(), 21);
        return rhythm(Markdown::instance()->plainHtml(payloadText()),
                      QStringLiteral("21px"));
    }

    void paintHeader(QPainter &painter, const QRect &content) override
    {
        const Theme *theme = Theme::instance();
        const QString body = payloadText();
        const QString caption = m_data.title.isEmpty() ? QString()
                                                       : producerCaption(m_data.title);

        FlowHeader head;
        head.icon = m_data.title.isEmpty() ? QStringLiteral("notice")
                                           : noticeIconFor(m_data.title);
        head.glyph = QStringLiteral("\u25cb");
        head.titlePixel = theme->fontData();
        head.titleColour = theme->caption();
        if (caption.isEmpty()) {
            head.title = firstLine(body);
        } else {
            head.title = caption;
            head.summary = firstLine(m_data.detail.isEmpty() ? body : m_data.detail);
            head.summaryFont = uiFont(theme->fontUI());
            head.summaryColour = theme->dim();
        }
        head.rightCaret = true;
        head.canDisclose = hasPayload();
        head.expanded = m_data.expanded;
        paintFlowHeader(painter, content, head);
    }
};

class ErrorRow : public ExpandedRow
{
public:
    explicit ErrorRow(QWidget *parent = nullptr)
        : ExpandedRow(RowClass::Error, parent)
    {
    }

protected:
    int headerPixels() const override { return 25; }

    QString payloadText() const override { return m_data.text; }

    QString payloadMarkup() const override
    {
        return rhythm(Markdown::instance()->plainHtml(payloadText()),
                      QStringLiteral("21px"));
    }

    QColor payloadColour() const override { return Theme::instance()->danger(); }

    void paintHeader(QPainter &painter, const QRect &content) override
    {
        const Theme *theme = Theme::instance();
        FlowHeader head;
        head.icon = QStringLiteral("error");
        head.title = firstLine(m_data.text);
        head.titlePixel = theme->fontUI();
        head.titleColour = theme->danger();
        head.rightCaret = true;
        head.canDisclose = hasPayload();
        head.expanded = m_data.expanded;
        paintFlowHeader(painter, content, head);
    }
};

class CompactedRow : public ExpandedRow
{
public:
    explicit CompactedRow(QWidget *parent = nullptr)
        : ExpandedRow(RowClass::Compacted, parent)
    {
    }

protected:
    int headerPixels() const override { return Theme::instance()->rowHeight(); }

    QString payloadText() const override { return m_data.detail; }

    QString payloadMarkup() const override
    {
        return rhythm(Markdown::instance()->toHtml(payloadText()), markdownRhythm());
    }

    void paintHeader(QPainter &painter, const QRect &content) override
    {
        const Theme *theme = Theme::instance();
        const int height = headerPixels();
        const bool running = m_data.status == QLatin1String("running")
            || m_data.status == QLatin1String("pending")
            || m_data.status == QLatin1String("in_progress");

        painter.setPen(Qt::NoPen);
        painter.setBrush(running ? theme->running() : theme->ok());
        painter.drawEllipse(QPointF(content.left() + 4.0, content.top() + height / 2.0),
                            4.0, 4.0);

        const QString label = m_data.title.isEmpty() ? m_data.text : m_data.title;
        const QString preview = m_data.title.isEmpty() ? QString() : m_data.text;
        const QString line = preview.isEmpty()
            ? label : label + QStringLiteral(" \u00b7 ") + preview;

        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        painter.setFont(font);
        painter.setPen(theme->dim());
        const int room = qMax(0, content.width() - 22 - 16);
        painter.drawText(QRect(content.left() + 22, content.top(), room, height),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(line, Qt::ElideRight, room));

        if (hasPayload()) {
            painter.setPen(theme->faint());
            painter.drawText(QRect(content.right() - 16, content.top(), 16, height),
                             Qt::AlignVCenter | Qt::AlignRight,
                             m_data.expanded ? QStringLiteral("\u25be")
                                             : QStringLiteral("\u25b8"));
        }
    }
};

class FlowRow : public TranscriptRow
{
public:
    explicit FlowRow(RowClass rowClass, QWidget *parent = nullptr)
        : TranscriptRow(rowClass, parent)
    {
        setMouseTracking(true);
    }

    void setRow(const RowData &data) override
    {
        const bool changed = data.text != m_data.text || data.detail != m_data.detail
            || data.request != m_data.request || data.title != m_data.title
            || data.status != m_data.status;
        TranscriptRow::setRow(data);
        if (changed)
            configure();
        updateGeometry();
        layoutBody();
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        return headerPixels() + payloadSlot(qMax(0, width));
    }

protected:
    virtual int headerPixels() const { return 25; }
    virtual int payloadSlot(int) const { return 0; }
    virtual bool canDisclose() const { return false; }
    virtual FlowHeader header() const { return FlowHeader(); }
    virtual void configure() {}
    virtual void paintPayload(QPainter &, const QRect &) {}

    bool hovered() const { return m_hover; }

    void paintEvent(QPaintEvent *) override
    {
        const QRect content = contentRect();
        if (content.isEmpty())
            return;
        QPainter painter(this);
        paintFlowHeader(painter,
                        QRect(content.left(), content.top(), content.width(), headerPixels()),
                        header());
        paintPayload(painter, content);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && canDisclose() && toggleExpanded) {
            toggleExpanded();
            return;
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

    void enterEvent(QEnterEvent *) override
    {
        m_hover = true;
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }

    bool m_hover = false;
};

class ToolScroll : public QTextEdit
{
public:
    explicit ToolScroll(QWidget *parent = nullptr)
        : QTextEdit(parent)
    {
        setReadOnly(true);
        setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setAcceptDrops(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setContentsMargins(0, 0, 0, 0);
        document()->setDocumentMargin(0);
        setAutoFillBackground(false);
        viewport()->setAutoFillBackground(false);

        QPalette colours = palette();
        colours.setColor(QPalette::Base, Qt::transparent);
        colours.setColor(QPalette::Window, Qt::transparent);
        colours.setColor(QPalette::Highlight, Theme::instance()->accent());
        colours.setColor(QPalette::HighlightedText, Theme::instance()->accentFg());
        setPalette(colours);

        QFont face = monoFont(Theme::instance()->fontData());
        setFont(face);
        document()->setDefaultFont(face);
    }

    void setMarkup(const QString &html, const QColor &colour)
    {
        QPalette colours = palette();
        colours.setColor(QPalette::Text, colour);
        setPalette(colours);
        setHtml(html);
        document()->setDocumentMargin(0);
    }

    int measure(int width) const
    {
        if (width <= 0)
            return 0;
        document()->setTextWidth(width);
        return qCeil(document()->size().height());
    }
};

class ToolCard : public QWidget
{
public:
    explicit ToolCard(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        m_in = new ToolScroll(this);
        m_out = new ToolScroll(this);
        m_in->hide();
        m_out->hide();
        setAutoFillBackground(false);
    }

    void setRow(const RowData &data)
    {
        m_data = data;
        const bool hasRequest = !data.request.isEmpty();
        const bool hasDetail = !data.detail.isEmpty();
        const bool failed = data.status == QLatin1String("failed")
            || data.status == QLatin1String("error");

        if (hasRequest) {
            m_in->setMarkup(rhythm(Markdown::instance()->plainHtml(data.request),
                                   QStringLiteral("17px")),
                            Theme::instance()->dim());
        }
        if (hasDetail) {
            m_out->setMarkup(isDiff() ? diffMarkup(data.detail, 17)
                                      : rhythm(Markdown::instance()->plainHtml(data.detail),
                                               QStringLiteral("17px")),
                            failed ? Theme::instance()->danger()
                                   : Theme::instance()->dim());
        }
        m_in->setVisible(hasRequest);
        m_out->setVisible(hasDetail);
        layoutHalves();
    }

    bool hasPayload() const
    {
        return !m_data.request.isEmpty() || !m_data.detail.isEmpty()
            || isDiff();
    }

    bool isDiff() const
    {
        const bool failed = m_data.status == QLatin1String("failed")
            || m_data.status == QLatin1String("error");
        const bool change = m_data.title == QLatin1String("edit")
            || m_data.title == QLatin1String("write");
        return change && !failed && !m_data.detail.isEmpty()
            && (m_data.detail.startsWith(QLatin1String("- "))
                || m_data.detail.startsWith(QLatin1String("+ ")));
    }

    int heightFor(int width) const
    {
        const int inH = inHeight(width);
        const int outH = outHeight(width);
        return inH + ((inH > 0 && outH > 0) ? Theme::instance()->hairline() : 0) + outH;
    }

    bool inVisible() const { return !m_data.request.isEmpty(); }
    bool outVisible() const { return !m_data.detail.isEmpty() || isDiff(); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        layoutHalves();
    }

    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF surface(0.5, 0.5, width() - 1.0, height() - 1.0);
        painter.setPen(QPen(theme->borderL1(), theme->hairline()));
        painter.setBrush(theme->codeBlock());
        painter.drawRoundedRect(surface, 12.0, 12.0);

        if (!m_data.status.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(stateColour());
            painter.drawEllipse(QPointF(16.0, 12.0), 4.0, 4.0);
        }

        const QFont labelFont = uiFont(theme->fontData());
        painter.setFont(labelFont);
        painter.setPen(theme->caption());
        const int labelX = 16 + siteWidth();
        if (inVisible())
            painter.drawText(QRect(labelX, 12, 80, 17),
                             Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("IN"));
        if (outVisible()) {
            const int y = inHeight(width())
                + ((inHeight(width()) > 0 && outHeight(width()) > 0)
                       ? theme->hairline() : 0);
            painter.drawText(QRect(labelX, y + 12, 80, 17),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             isDiff() ? QStringLiteral("DIFF") : QStringLiteral("OUT"));
        }

        if (inVisible() && outVisible()) {
            painter.setPen(theme->borderL2());
            const int y = inHeight(width());
            painter.drawLine(0, y, width(), y);
        }
    }

private:
    int siteWidth() const
    {
        return m_data.status.isEmpty() ? 0 : 8 + Theme::instance()->space3();
    }

    int gutterWidth() const
    {
        const QFontMetrics metrics(uiFont(Theme::instance()->fontData()));
        return qMax(metrics.horizontalAdvance(QStringLiteral("IN")),
                    metrics.horizontalAdvance(QStringLiteral("DIFF")));
    }

    int contentX() const
    {
        return 16 + siteWidth() + gutterWidth() + 14;
    }

    int halfWidth(int width) const
    {
        return qMax(0, width - contentX() - 16);
    }

    int inHeight(int width) const
    {
        if (m_data.request.isEmpty())
            return 0;
        return 24 + qMin(m_in->measure(halfWidth(width)), 150);
    }

    int outHeight(int width) const
    {
        if (m_data.detail.isEmpty() && !isDiff())
            return 0;
        return 24 + qMin(m_out->measure(halfWidth(width)), 150);
    }

    QColor stateColour() const
    {
        const QString status = m_data.status;
        if (status == QLatin1String("in_progress") || status == QLatin1String("pending")
                || status == QLatin1String("running"))
            return Theme::instance()->running();
        if (status == QLatin1String("failed") || status == QLatin1String("error"))
            return Theme::instance()->danger();
        if (status == QLatin1String("completed") || status == QLatin1String("complete"))
            return Theme::instance()->ok();
        return Theme::instance()->warn();
    }

    void layoutHalves()
    {
        const int widthNow = width();
        const int inH = inHeight(widthNow);
        const int outH = outHeight(widthNow);
        const int x = contentX();
        const int half = halfWidth(widthNow);
        if (inH > 0) {
            m_in->setGeometry(x, 12, half, qMin(m_in->measure(half), 150));
            m_in->show();
        } else {
            m_in->hide();
        }
        if (outH > 0) {
            const int y = inH + ((inH > 0 && outH > 0) ? Theme::instance()->hairline() : 0);
            m_out->setGeometry(x, y + 12, half, qMin(m_out->measure(half), 150));
            m_out->show();
        } else {
            m_out->hide();
        }
    }

    RowData m_data;
    ToolScroll *m_in = nullptr;
    ToolScroll *m_out = nullptr;
};

class ToolRow : public FlowRow
{
public:
    explicit ToolRow(QWidget *parent = nullptr)
        : FlowRow(RowClass::Tool, parent)
    {
        m_card = new ToolCard(this);
        m_card->hide();
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        int total = headerPixels();
        if (payloadVisible())
            total += Theme::instance()->space2() + m_card->heightFor(qMax(1, width - 4));
        if (!toolErrorText().isEmpty())
            total += 23;
        return total;
    }

protected:
    bool canDisclose() const override { return m_card->hasPayload(); }

    void configure() override
    {
        m_card->setRow(m_data);
        m_card->setVisible(payloadVisible());
    }

    void layoutBody() override
    {
        const QRect content = contentRect();
        if (content.isEmpty()) {
            m_card->hide();
            return;
        }
        const bool show = payloadVisible();
        m_card->setVisible(show);
        if (!show)
            return;
        const int cardWidth = qMax(1, content.width() - 4);
        m_card->setGeometry(4, content.top() + headerPixels() + Theme::instance()->space2(),
                            cardWidth, m_card->heightFor(cardWidth));
    }

    FlowHeader header() const override
    {
        const Theme *theme = Theme::instance();
        const bool failed = toolFailed();
        const bool brand = isBrandTool();
        FlowHeader head;
        head.icon = toolIconFor(m_data.title);
        head.glyph = toolGlyphFor(m_data.title);
        head.title = displayToolTitle(m_data.title);
        head.titlePixel = theme->fontUI();
        head.titleColour = failed ? theme->danger()
            : brand ? theme->accent() : theme->dim();
        head.medium = brand;
        head.summary = firstLine(m_data.text);
        head.summaryFont = monoFont(theme->fontData());
        head.summaryColour = failed ? theme->danger() : theme->caption();
        head.statusWord = failed ? m_data.status : QString();
        head.chevron = canDisclose() && (m_data.expanded || hovered());
        head.canDisclose = canDisclose();
        head.expanded = m_data.expanded;
        return head;
    }

    void paintPayload(QPainter &painter, const QRect &content) override
    {
        const QString error = toolErrorText();
        if (error.isEmpty())
            return;
        const Theme *theme = Theme::instance();
        const int cardSlot = payloadVisible()
            ? Theme::instance()->space2() + m_card->heightFor(qMax(1, content.width() - 4))
            : 0;
        const QRect lineRect(content.left() + 22,
                             content.top() + headerPixels() + cardSlot + 2,
                             qMax(0, content.width() - 22), 21);
        painter.setFont(uiFont(theme->fontProse()));
        painter.setPen(theme->danger());
        const QFontMetrics metrics(uiFont(theme->fontProse()));
        painter.drawText(lineRect, Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(error, Qt::ElideRight, lineRect.width()));
    }

private:
    bool payloadVisible() const
    {
        return m_data.expanded && m_card->hasPayload();
    }

    bool toolFailed() const
    {
        return m_data.status == QLatin1String("failed")
            || m_data.status == QLatin1String("error");
    }

    bool isBrandTool() const
    {
        return m_data.title.toLower().startsWith(QLatin1String("cordis"));
    }

    QString toolErrorText() const
    {
        if (!toolFailed() || m_data.detail.isEmpty())
            return QString();
        return firstLine(m_data.detail);
    }

    ToolCard *m_card = nullptr;
};

class ThoughtRow : public FlowRow
{
public:
    explicit ThoughtRow(QWidget *parent = nullptr)
        : FlowRow(RowClass::Thought, parent)
    {
        m_prose = new ProseEdit(this);
        m_prose->hide();
    }

    bool hasContent() const override { return !m_data.text.isEmpty(); }

    int bodyHeight(int width) const override
    {
        if (!hasContent())
            return 0;
        int total = headerPixels();
        if (payloadVisible())
            total += Theme::instance()->space2() + m_prose->measure(payloadWidth(width));
        return total;
    }

protected:
    bool canDisclose() const override { return hasContent(); }

    void configure() override
    {
        if (!hasContent()) {
            m_prose->hide();
            return;
        }
        const QString markup = m_data.status == QLatin1String("diff")
            ? diffMarkup(m_data.text, 21)
            : rhythm(Markdown::instance()->plainHtml(m_data.text), QStringLiteral("21px"));
        m_prose->setMarkup(markup, Theme::instance()->dim(),
                           Theme::instance()->fontProse(), false);
    }

    void layoutBody() override
    {
        const QRect content = contentRect();
        if (content.isEmpty() || !payloadVisible()) {
            m_prose->hide();
            return;
        }
        const int width = payloadWidth(content.width());
        const int height = m_prose->measure(width);
        m_prose->show();
        m_prose->setGeometry(22, content.top() + headerPixels() + Theme::instance()->space2(),
                             width, height);
    }

    FlowHeader header() const override
    {
        const Theme *theme = Theme::instance();
        FlowHeader head;
        head.icon = QStringLiteral("think");
        head.glyph = QStringLiteral("\u2726");
        head.title = QStringLiteral("Think");
        head.titlePixel = theme->fontUI();
        head.titleColour = theme->dim();
        head.summary = firstLine(m_data.text);
        head.summaryFont = uiFont(theme->fontData());
        head.summaryColour = theme->caption();
        head.chevron = canDisclose() && (m_data.expanded || hovered());
        head.canDisclose = canDisclose();
        head.expanded = m_data.expanded;
        return head;
    }

private:
    bool payloadVisible() const { return m_data.expanded && hasContent(); }

    int payloadWidth(int width) const { return qMax(1, width - 22); }

    ProseEdit *m_prose = nullptr;
};

class ImageRow : public TranscriptRow
{
public:
    explicit ImageRow(QWidget *parent = nullptr)
        : TranscriptRow(RowClass::Image, parent)
    {
        setCursor(Qt::PointingHandCursor);
    }

    void setRow(const RowData &data) override
    {
        const bool changed = data.detail != m_data.detail || data.text != m_data.text;
        TranscriptRow::setRow(data);
        if (changed)
            m_picture = QPixmap();
        if (m_picture.isNull())
            refreshPicture();
        updateGeometry();
        layoutBody();
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        Q_UNUSED(width)
        const QFontMetrics metrics(uiFont(Theme::instance()->fontData()));
        return kFrameHeight
            + (m_data.text.isEmpty() ? 0 : Theme::instance()->space3() + metrics.height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QRect content = contentRect();
        if (content.isEmpty())
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF frame(content.left(), content.top(), content.width(), kFrameHeight);
        QPainterPath clip;
        clip.addRoundedRect(frame, theme->radius(), theme->radius());
        painter.save();
        painter.setClipPath(clip);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme->raised());
        painter.drawRect(frame);
        if (!m_picture.isNull()) {
            QSize shown = m_picture.size();
            shown.scale(qMax(1, content.width() - 2 * theme->hairline()),
                        qMax(1, kFrameHeight - 2 * theme->hairline()),
                        Qt::KeepAspectRatio);
            const QRect target(content.left() + theme->hairline(),
                               content.top() + theme->hairline()
                                   + (kFrameHeight - 2 * theme->hairline() - shown.height()) / 2,
                               shown.width(), shown.height());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawPixmap(target, m_picture);
        }
        painter.restore();

        painter.setPen(QPen(theme->border(), theme->hairline()));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5),
                                theme->radius(), theme->radius());

        if (m_data.text.isEmpty())
            return;
        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        painter.setFont(font);
        painter.setPen(theme->dim());
        const QRect caption(content.left(),
                            content.top() + kFrameHeight + theme->space3(),
                            content.width(), metrics.height());
        painter.drawText(caption, Qt::AlignTop | Qt::AlignLeft,
                         metrics.elidedText(firstLine(m_data.text), Qt::ElideRight,
                                            caption.width()));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !m_picture.isNull() && openImage) {
            const QString source = attachmentUrl ? attachmentUrl(m_data.detail) : QString();
            if (!source.isEmpty()) {
                openImage(source, m_data.text, m_picture.width(), m_picture.height());
                return;
            }
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

private:
    static constexpr int kFrameHeight = 240;

    void refreshPicture()
    {
        if (m_data.detail.isEmpty() || !attachmentPicture)
            return;
        if (loadAttachment)
            loadAttachment(m_data.detail);
        m_picture = attachmentPicture(m_data.detail);
    }

    QPixmap m_picture;
};

class FileRow : public TranscriptRow
{
public:
    explicit FileRow(QWidget *parent = nullptr)
        : TranscriptRow(RowClass::File, parent)
    {
        setMouseTracking(true);
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        Q_UNUSED(width)
        return Theme::instance()->rowHeight();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QRect content = contentRect();
        if (content.isEmpty())
            return;

        QPainter painter(this);
        const int height = theme->rowHeight();
        const QRect row(content.left(), content.top(), content.width(), height);
        const bool openable = m_data.status == QLatin1String("openable");

        const QPixmap mark = markIcon(openable ? QStringLiteral("file-write")
                                               : QStringLiteral("file-read"));
        const int markSize = 16;
        if (!mark.isNull())
            painter.drawPixmap(QRect(row.left(), row.top() + (height - markSize) / 2,
                                     markSize, markSize), mark);

        const QFont uiFace = uiFont(theme->fontData());
        const QFontMetrics uiMetrics(uiFace);
        const QFont monoFace = monoFont(theme->fontMono());
        const QFontMetrics monoMetrics(monoFace);

        int right = row.right();
        if (openable) {
            painter.setFont(uiFace);
            painter.setPen(m_hover ? theme->accent() : theme->dim());
            const int openWidth = uiMetrics.horizontalAdvance(QStringLiteral("Open"));
            painter.drawText(QRect(right - openWidth, row.top(), openWidth, height),
                             Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("Open"));
            right -= openWidth + theme->space4();
        }

        if (!m_data.title.isEmpty()) {
            painter.setFont(uiFace);
            painter.setPen(theme->dim());
            const int metaWidth = qMin(uiMetrics.horizontalAdvance(m_data.title),
                                       qRound(row.width() * 0.45));
            painter.drawText(QRect(right - metaWidth, row.top(), metaWidth, height),
                             Qt::AlignVCenter | Qt::AlignRight,
                             uiMetrics.elidedText(m_data.title, Qt::ElideRight, metaWidth));
            right -= metaWidth + theme->space4();
        }

        const int nameX = row.left() + markSize + theme->space3();
        const int nameWidth = qMax(0, right - nameX);
        painter.setFont(monoFace);
        painter.setPen(theme->fg());
        painter.drawText(QRect(nameX, row.top(), nameWidth, height),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         monoMetrics.elidedText(m_data.text, Qt::ElideLeft, nameWidth));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton
                && m_data.status == QLatin1String("openable") && openFile) {
            openFile(m_data.text);
            return;
        }
        TranscriptRow::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        setCursor(m_data.status == QLatin1String("openable")
                      ? Qt::PointingHandCursor : Qt::ArrowCursor);
        const bool hover = m_data.status == QLatin1String("openable")
            && contentRect().contains(event->position().toPoint());
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        TranscriptRow::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }

private:
    bool m_hover = false;
};

class MessageFileRow : public TranscriptRow
{
public:
    explicit MessageFileRow(QWidget *parent = nullptr)
        : TranscriptRow(RowClass::MessageFile, parent)
    {
    }

    bool hasContent() const override { return true; }

    int bodyHeight(int width) const override
    {
        Q_UNUSED(width)
        return Theme::instance()->rowHeight();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QRect content = contentRect();
        if (content.isEmpty())
            return;

        const int hpad = theme->space3();
        const int markSize = theme->space6();
        const QFont face = uiFont(theme->fontData());
        const QFontMetrics metrics(face);
        const qreal nameNatural = metrics.horizontalAdvance(m_data.text);
        const qreal metaNatural = metrics.horizontalAdvance(m_data.title);
        const qreal fixed = 2 * hpad + markSize + theme->space3()
            + (metaNatural > 0 ? theme->space2() : 0);
        const qreal bubbleMax = qMin(content.width() * 0.82, 680.0);
        const qreal room = qMax(0.0, qMin<qreal>(bubbleMax, content.width()) - fixed);
        const bool fits = nameNatural + metaNatural <= room;
        const qreal metaWidth = fits ? metaNatural : qMin(metaNatural, room * 0.5);
        const qreal nameWidth = fits ? nameNatural : qMax(0.0, room - metaWidth);
        const qreal chipWidth = fixed + nameWidth + metaWidth;
        const QRectF chip(content.right() - chipWidth, content.top(),
                          chipWidth, theme->rowHeight());

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(theme->border(), theme->hairline()));
        painter.setBrush(theme->raised());
        painter.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5),
                                theme->radius(), theme->radius());

        const QPixmap mark = markIcon(QStringLiteral("file-read"));
        if (!mark.isNull()) {
            painter.drawPixmap(QRect(qRound(chip.left()) + hpad,
                                     qRound(chip.top()) + (theme->rowHeight() - markSize) / 2,
                                     markSize, markSize), mark);
        }

        painter.setFont(face);
        painter.setPen(theme->fg());
        const int nameX = qRound(chip.left()) + hpad + markSize + theme->space3();
        painter.drawText(QRect(nameX, qRound(chip.top()), qRound(nameWidth), theme->rowHeight()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(m_data.text, Qt::ElideMiddle, qRound(nameWidth)));

        if (metaWidth > 0 && !m_data.title.isEmpty()) {
            painter.setPen(theme->dim());
            const int metaX = nameX + qRound(nameWidth)
                + (metaWidth > 0 ? theme->space2() : 0);
            painter.drawText(QRect(metaX, qRound(chip.top()), qRound(metaWidth),
                                   theme->rowHeight()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             metrics.elidedText(m_data.title, Qt::ElideRight,
                                                qRound(metaWidth)));
        }
    }
};

TranscriptRow *makeRow(QWidget *parent, const RowData &data)
{
    TranscriptRow *row = nullptr;
    switch (rowClassFor(data)) {
    case RowClass::User:
        row = new UserRow(parent);
        break;
    case RowClass::Agent:
        row = new AgentRow(parent);
        break;
    case RowClass::Notice:
        row = new NoticeRow(parent);
        break;
    case RowClass::Error:
        row = new ErrorRow(parent);
        break;
    case RowClass::Compacted:
        row = new CompactedRow(parent);
        break;
    case RowClass::Tool:
        row = new ToolRow(parent);
        break;
    case RowClass::Thought:
        row = new ThoughtRow(parent);
        break;
    case RowClass::Image:
        row = new ImageRow(parent);
        break;
    case RowClass::File:
        row = new FileRow(parent);
        break;
    case RowClass::MessageFile:
        row = new MessageFileRow(parent);
        break;
    case RowClass::PendingSend:
        row = new UserRow(parent, RowClass::PendingSend);
        break;
    case RowClass::Placeholder:
        row = new PlaceholderRow(parent);
        break;
    }
    row->setRow(data);
    return row;
}

class TranscriptPagingHeader : public QWidget
{
public:
    explicit TranscriptPagingHeader(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setMouseTracking(true);
        setFixedHeight(0);
    }

    void setState(bool hasEarlier, bool loading, bool atStart)
    {
        m_hasEarlier = hasEarlier;
        m_loading = loading;
        m_atStart = atStart;
        const int wanted = (m_hasEarlier || m_atStart) ? 40 : 0;
        if (height() != wanted)
            setFixedHeight(wanted);
        update();
    }

    std::function<void()> activated;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme *theme = Theme::instance();
        const QFont font = uiFont(theme->fontData());
        const QFontMetrics metrics(font);
        QPainter painter(this);
        painter.setFont(font);

        if (m_hasEarlier) {
            const QString label = m_loading ? QStringLiteral("Loading earlier history\u2026")
                                            : QStringLiteral("Scroll up for earlier history");
            const int chipWidth = metrics.horizontalAdvance(label) + 2 * theme->space4();
            const int chipHeight = 27;
            const QRect chip(theme->space5(), (height() - chipHeight) / 2, chipWidth, chipHeight);
            painter.setPen(QPen(theme->borderL2(), theme->hairline()));
            painter.setBrush(theme->raised());
            painter.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5),
                                    chipHeight / 2.0, chipHeight / 2.0);
            painter.setPen(theme->dim());
            painter.drawText(chip, Qt::AlignCenter, label);
        } else if (m_atStart) {
            painter.setPen(theme->dim());
            painter.drawText(QRect(theme->space5(), 0, width() - theme->space5(), height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             QStringLiteral("Start of session"));
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_hasEarlier && !m_loading && activated
                && chipRect().contains(event->position().toPoint())) {
            activated();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_hasEarlier && !m_loading)
            setCursor(chipRect().contains(event->position().toPoint())
                          ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

private:
    QRect chipRect() const
    {
        const Theme *theme = Theme::instance();
        const QFontMetrics metrics(uiFont(theme->fontData()));
        const QString label = m_loading ? QStringLiteral("Loading earlier history\u2026")
                                        : QStringLiteral("Scroll up for earlier history");
        return QRect(theme->space5(), (height() - 27) / 2,
                     metrics.horizontalAdvance(label) + 2 * theme->space4(), 27);
    }

    bool m_hasEarlier = false;
    bool m_loading = false;
    bool m_atStart = false;
};

class AttachmentStore
{
public:
    explicit AttachmentStore(ClientFacade *facade)
        : m_facade(facade)
    {
    }

    QString url(const QString &id)
    {
        if (id.isEmpty() || !m_facade || !m_facade->transport())
            return QString();
        QObject *transport = m_facade->transport();
        QString result;
        if (!QMetaObject::invokeMethod(transport, "attachmentUrl", Qt::DirectConnection,
                                       Q_RETURN_ARG(QString, result), Q_ARG(QString, id)))
            return QString();
        if (result.isEmpty())
            request(id);
        return result;
    }

    void request(const QString &id)
    {
        if (id.isEmpty() || !m_facade || !m_facade->transport())
            return;
        if (m_requested.contains(id))
            return;
        m_requested.insert(id);
        QMetaObject::invokeMethod(m_facade->transport(), "loadAttachment",
                                  Qt::DirectConnection, Q_ARG(QString, id));
    }

    QPixmap picture(const QString &id)
    {
        if (m_pixmaps.contains(id))
            return m_pixmaps.value(id);
        const QString source = url(id);
        QPixmap pixmap;
        if (source.startsWith(QLatin1String("data:"))) {
            const int comma = source.indexOf(QLatin1Char(','));
            if (comma > 0) {
                const QByteArray payload =
                    QByteArray::fromBase64(source.mid(comma + 1).toLatin1());
                QImage image;
                if (image.loadFromData(payload))
                    pixmap = QPixmap::fromImage(image);
            }
        } else if (!source.isEmpty()) {
            if (source.startsWith(QLatin1String("file:")))
                pixmap = QPixmap(QUrl(source).toLocalFile());
            else
                pixmap = QPixmap(source);
        }
        m_pixmaps.insert(id, pixmap);
        return pixmap;
    }

    void clear() { m_pixmaps.clear(); }

private:
    ClientFacade *m_facade = nullptr;
    QSet<QString> m_requested;
    QHash<QString, QPixmap> m_pixmaps;
};

TranscriptView::TranscriptView(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    setObjectName(QStringLiteral("transcriptView"));
    m_attachments = new AttachmentStore(facade);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("transcriptScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QPalette scrollPalette = m_scroll->palette();
    scrollPalette.setColor(QPalette::Base, Theme::instance()->bg());
    scrollPalette.setColor(QPalette::Window, Theme::instance()->bg());
    m_scroll->setPalette(scrollPalette);
    m_scroll->viewport()->setAutoFillBackground(true);
    Chrome::styleScrollBar(m_scroll->verticalScrollBar());
    layout->addWidget(m_scroll);

    m_content = new QWidget(m_scroll->viewport());
    m_content->setObjectName(QStringLiteral("transcriptColumn"));
    m_content->setAutoFillBackground(false);
    m_column = new QVBoxLayout(m_content);
    m_column->setContentsMargins(Theme::instance()->space6(), Theme::instance()->space6(),
                                 Theme::instance()->space6(), 0);
    m_column->setSpacing(0);
    m_paging = new TranscriptPagingHeader(m_content);
    m_paging->activated = [this]() {
        if (!m_facade || m_facade->loadingEarlier())
            return;
        m_pagedEarlier = true;
        beginEarlierPage();
        emit pageEarlier();
        m_facade->loadEarlierHistory();
    };
    m_column->addWidget(m_paging);
    m_column->addStretch(1);
    m_scroll->setWidget(m_content);
    m_content->installEventFilter(this);

    m_empty = new QLabel(m_scroll->viewport());
    m_empty->setObjectName(QStringLiteral("transcriptEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setFont(uiFont(Theme::instance()->fontProse()));
    QPalette emptyPalette = m_empty->palette();
    emptyPalette.setColor(QPalette::WindowText, Theme::instance()->dim());
    m_empty->setPalette(emptyPalette);
    m_empty->hide();
    m_empty->raise();

    m_scroll->viewport()->installEventFilter(this);

    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, &TranscriptView::onScrollValueChanged);
    connect(bar, &QScrollBar::rangeChanged, this, &TranscriptView::onScrollRangeChanged);

    m_glide = new QPropertyAnimation(bar, "value", this);
    m_glide->setDuration(140);
    m_glide->setEasingCurve(QEasingCurve::OutCubic);
    connect(bar, &QScrollBar::sliderPressed, m_glide, &QPropertyAnimation::stop);

    if (m_facade) {
        connect(m_facade, &ClientFacade::transcriptChanged, this, [this]() {
            setModel(m_facade->transcript());
        });
        if (QObject *transport = m_facade->transport()) {
            if (transport->metaObject()->indexOfSignal("attachmentsChanged()") >= 0) {
                connect(transport, SIGNAL(attachmentsChanged()),
                        this, SLOT(onAttachmentsChanged()));
            }
        }
        connect(m_facade, &ClientFacade::hasEarlierChanged,
                this, &TranscriptView::refreshPagingHeader);
        connect(m_facade, &ClientFacade::stateChanged,
                this, &TranscriptView::refreshEmptyState);
        connect(m_facade, &ClientFacade::lastErrorChanged,
                this, &TranscriptView::refreshEmptyState);
        connect(m_facade, &ClientFacade::openingSessionChanged,
                this, &TranscriptView::refreshEmptyState);
        connect(m_facade, &ClientFacade::pendingSessionChanged,
                this, &TranscriptView::refreshEmptyState);
    }

    setModel(m_facade ? m_facade->transcript() : nullptr);
    refreshPagingHeader();
    refreshEmptyState();
    layoutEmptyState();
}

TranscriptView::~TranscriptView()
{
    for (TranscriptRow *row : m_rows) {
        if (!row)
            continue;
        row->attachmentUrl = nullptr;
        row->loadAttachment = nullptr;
        row->attachmentPicture = nullptr;
    }
    m_rows.clear();
    delete m_attachments;
    m_attachments = nullptr;
}

int TranscriptView::rowCount() const
{
    return m_rows.size();
}

QWidget *TranscriptView::rowAt(int index) const
{
    return index >= 0 && index < m_rows.size() ? m_rows.at(index) : nullptr;
}

int TranscriptView::contentY() const
{
    return m_scroll->verticalScrollBar()->value();
}

int TranscriptView::contentHeight() const
{
    return m_content->height();
}

int TranscriptView::viewportHeight() const
{
    return m_scroll->viewport()->height();
}

void TranscriptView::scrollToEnd()
{
    m_followTail = true;
    if (m_glide)
        m_glide->stop();
    QTimer::singleShot(0, this, [this]() { applyFollow(); });
}

void TranscriptView::bindRow(TranscriptRow *row)
{
    row->toggleExpanded = [this, row]() { toggleExpanded(row); };
    row->openImage = [this](const QVariant &source, const QVariant &alt,
                            double fallbackWidth, double fallbackHeight) {
        emit openImage(source, alt, fallbackWidth, fallbackHeight);
    };
    row->openFile = [this](const QString &path) { emit openFile(path); };
    row->attachmentUrl = [this](const QString &id) { return m_attachments->url(id); };
    row->loadAttachment = [this](const QString &id) { m_attachments->request(id); };
    row->attachmentPicture = [this](const QString &id) {
        return m_attachments->picture(id);
    };
    row->setRow(row->row());
}

void TranscriptView::onAttachmentsChanged()
{
    m_attachments->clear();
    for (int i = 0; i < m_rows.size(); ++i)
        refreshRow(i);
    syncColumnHeight();
}

void TranscriptView::setModel(QAbstractItemModel *model)
{
    if (m_model == model) {
        refreshEmptyState();
        refreshPagingHeader();
        return;
    }

    if (m_model)
        m_model->disconnect(this);
    m_model = model;

    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted,
                this, &TranscriptView::insertRows);
        connect(m_model, &QAbstractItemModel::rowsRemoved,
                this, &TranscriptView::removeRows);
        connect(m_model, &QAbstractItemModel::rowsMoved,
                this, &TranscriptView::rebuild);
        connect(m_model, &QAbstractItemModel::modelReset,
                this, &TranscriptView::rebuild);
        connect(m_model, &QAbstractItemModel::layoutChanged,
                this, &TranscriptView::rebuild);
        connect(m_model, &QAbstractItemModel::dataChanged,
                this, &TranscriptView::updateRows);
    }

    rebuild();
}

void TranscriptView::rebuild()
{
    m_expandedRows.clear();
    m_pagedEarlier = false;
    m_historyStart = false;
    m_earlierBrace = false;
    m_earlierPageArrived = false;
    m_earlierArmed = true;
    m_followTail = true;
    rebuildRows();
    refreshPagingHeader();
    refreshEmptyState();
    QTimer::singleShot(0, this, [this]() { if (m_followTail) applyFollow(); });
}

void TranscriptView::rebuildRows()
{
    ++m_buildGeneration;
    m_buildQueued = false;
    m_waveActive = false;
    beginBuildWave(true);

    for (TranscriptRow *row : m_rows) {
        if (!row)
            continue;
        m_column->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    m_rows.clear();
    m_rowHeights.clear();
    m_measuredWidth = -1;

    const int count = m_model ? m_model->rowCount() : 0;
    m_rows = QList<TranscriptRow *>(count, nullptr);
    m_rowHeights = QList<int>(count, -1);
    m_builtStart = count;
    m_tailStart = count;
    m_tailNext = -1;

    syncColumnHeight();
    buildSlice();
}

bool TranscriptView::buildPending() const
{
    if (m_builtStart > 0)
        return true;
    return m_tailStart < m_rows.size() && m_tailNext >= m_tailStart;
}

void TranscriptView::beginBuildWave(bool rebuild)
{
    if (m_waveActive)
        return;
    m_waveActive = true;
    m_waveRebuild = rebuild;
    m_waveBuilt = 0;
    m_buildStartedMs = QDateTime::currentMSecsSinceEpoch();
}

void TranscriptView::logBuildComplete()
{
    const qint64 elapsed = m_buildStartedMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_buildStartedMs
        : 0;
    if (m_waveRebuild || m_waveBuilt >= 8) {
        qInfo().nospace() << (m_waveRebuild ? "transcript: rebuilt " : "transcript: built ")
                          << m_rows.size() << " rows in " << elapsed
                          << " ms, column " << (m_content ? m_content->height() : 0)
                          << "px, viewport " << (m_scroll ? m_scroll->viewport()->height() : 0)
                          << "px, scroll max "
                          << (m_scroll ? m_scroll->verticalScrollBar()->maximum() : -1)
                          << " value "
                          << (m_scroll ? m_scroll->verticalScrollBar()->value() : -1)
                          << ", tail " << (m_followTail ? "pinned" : "released");
        int tallest[3] = {-1, -1, -1};
        for (int i = 0; i < m_rowHeights.size() && i < m_rows.size(); ++i) {
            const int h = m_rowHeights.at(i);
            for (int slot = 0; slot < 3; ++slot) {
                if (tallest[slot] < 0 || h > m_rowHeights.at(tallest[slot])) {
                    for (int move = 2; move > slot; --move)
                        tallest[move] = tallest[move - 1];
                    tallest[slot] = i;
                    break;
                }
            }
        }
        QHash<QString, int> kinds;
        for (TranscriptRow *row : m_rows) {
            if (row)
                ++kinds[row->row().kind];
        }
        QStringList tally;
        for (auto it = kinds.constBegin(); it != kinds.constEnd(); ++it)
            tally << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        tally.sort();
        qInfo().noquote() << "transcript: kinds" << tally.join(QLatin1Char(' '));
        for (int slot = 0; slot < 3; ++slot) {
            const int index = tallest[slot];
            if (index < 0)
                continue;
            qInfo().nospace() << "transcript: row " << index << " kind "
                              << (m_rows.at(index) ? m_rows.at(index)->row().kind
                                                   : QStringLiteral("?"))
                              << " height " << m_rowHeights.at(index) << "px";
        }
    }
    m_waveActive = false;
    m_waveRebuild = false;
    m_waveBuilt = 0;
    m_buildStartedMs = 0;
}

void TranscriptView::scheduleBuild()
{
    if (m_buildQueued || !buildPending())
        return;
    m_buildQueued = true;
    const int generation = m_buildGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != m_buildGeneration)
            return;
        buildSlice();
    });
}

int TranscriptView::layoutIndexFor(int index) const
{
    const int stop = qMin(index, m_rows.size());
    int builtBefore = 0;
    for (int i = 0; i < stop; ++i) {
        if (m_rows.at(i))
            ++builtBefore;
    }
    return 1 + builtBefore;
}

void TranscriptView::buildRowsNow(int first, int last)
{
    const int width = columnWidth();
    const int layoutIndex = layoutIndexFor(first);
    for (int i = first; i <= last; ++i) {
        if (m_rows.at(i))
            continue;
        TranscriptRow *row = makeRow(m_content, rowDataFor(m_model, m_expandedRows, i));
        bindRow(row);
        row->setColumnWidth(width);
        m_column->insertWidget(layoutIndex + (i - first), row);
        m_rows[i] = row;
        m_rowHeights[i] = -1;
    }
}

void TranscriptView::settleBuildSlice(int beforeValue, int grownAbove)
{
    if (!m_scroll)
        return;
    if (m_content)
        m_content->update();
    QScrollBar *bar = m_scroll->verticalScrollBar();
    if (m_followTail) {
        applyFollow();
        return;
    }
    if (grownAbove <= 0)
        return;
    m_pinning = true;
    bar->setValue(qMin(beforeValue + grownAbove, bar->maximum()));
    m_pinning = false;
    m_lastValue = bar->value();
}

void TranscriptView::buildSlice()
{
    m_buildQueued = false;
    if (!m_model || !m_content || !m_column)
        return;

    if (m_tailStart < m_rows.size() && m_tailNext < m_tailStart)
        m_tailStart = m_rows.size();
    if (!buildPending()) {
        syncColumnHeight();
        if (m_followTail)
            applyFollow();
        logBuildComplete();
        return;
    }

    QScrollBar *bar = m_scroll->verticalScrollBar();
    const int beforeValue = bar->value();
    const int width = columnWidth();
    const int viewport = m_scroll->viewport()->height();
    int builtHeight = m_content->height();
    int grownAbove = 0;
    int builtRows = 0;
    QElapsedTimer slice;
    slice.start();

    for (;;) {
        if (builtRows > 0) {
            if (builtHeight < viewport) {
                if (builtHeight >= viewport || builtRows >= 80 || slice.elapsed() >= 120)
                    break;
            } else if (slice.elapsed() >= BuildSliceBudgetMs) {
                break;
            }
        }

        int index = -1;
        int layoutIndex = 0;
        bool above = false;
        if (m_tailStart < m_rows.size() && m_tailNext >= m_tailStart) {
            index = m_tailNext--;
            layoutIndex = 1 + (m_tailStart - m_builtStart);
        } else {
            if (m_tailStart < m_rows.size())
                m_tailStart = m_rows.size();
            if (m_builtStart <= 0)
                break;
            index = --m_builtStart;
            layoutIndex = 1;
            above = true;
        }

        if (m_rows.at(index))
            continue;

        TranscriptRow *row = makeRow(m_content, rowDataFor(m_model, m_expandedRows, index));
        bindRow(row);
        row->setColumnWidth(width);
        m_column->insertWidget(layoutIndex, row);
        m_rows[index] = row;
        const int rowHeight = row->heightForWidth(width);
        m_rowHeights[index] = rowHeight;
        builtHeight += rowHeight;
        if (above)
            grownAbove += rowHeight;
        ++builtRows;
    }

    m_waveBuilt += builtRows;
    syncColumnHeight();
    settleBuildSlice(beforeValue, grownAbove);

    if (m_tailStart < m_rows.size() && m_tailNext < m_tailStart)
        m_tailStart = m_rows.size();
    if (buildPending())
        scheduleBuild();
    else
        logBuildComplete();
}

void TranscriptView::insertRows(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid() || !m_model || first < 0 || last < first || last >= m_model->rowCount()) {
        rebuild();
        return;
    }

    if (first == 0 && m_earlierBrace) {
        if (!m_earlierPageArrived) {
            m_braceValue = m_scroll->verticalScrollBar()->value();
            m_braceHeight = m_content->height();
            m_earlierPageArrived = true;
        }
    }

    const int n = last - first + 1;
    const int oldCount = m_rows.size();
    if (m_tailStart < oldCount && m_tailNext < m_tailStart)
        m_tailStart = oldCount;
    const bool tailLive = m_tailStart < oldCount;

    m_rows.insert(first, n, nullptr);
    m_rowHeights.insert(first, n, -1);

    bool buildNow = false;
    if (first < m_builtStart) {
        m_builtStart += n;
        if (tailLive) {
            m_tailStart += n;
            m_tailNext += n;
        } else {
            m_tailStart = m_rows.size();
        }
    } else if (!tailLive) {
        if (first == oldCount) {
            m_tailStart = first;
            m_tailNext = m_rows.size() - 1;
        } else {
            buildNow = true;
            m_tailStart = m_rows.size();
        }
    } else if (first < m_tailStart) {
        buildNow = true;
        m_tailStart += n;
        m_tailNext += n;
    } else if (first == oldCount) {
        if (m_tailNext == oldCount - 1) {
            m_tailNext = m_rows.size() - 1;
        } else {
            buildNow = true;
        }
    } else if (first <= m_tailNext) {
        m_tailNext += n;
    } else {
        buildNow = true;
    }

    if (buildNow)
        buildRowsNow(first, last);

    for (int i = first; i < m_rows.size(); ++i) {
        if (i <= last)
            continue;
        refreshRow(i);
    }
    if (first > 0)
        refreshRow(first - 1);

    scheduleColumnHeight();
    if (buildPending()) {
        beginBuildWave(false);
        scheduleBuild();
    }
    refreshEmptyState();
    if (m_earlierBrace)
        QTimer::singleShot(0, this, [this]() { holdEarlierPlace(); });
}

void TranscriptView::removeRows(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid() || first < 0 || last < first || last >= m_rows.size()) {
        rebuild();
        return;
    }

    const bool tailLive = m_tailStart < m_rows.size() && m_tailNext >= m_tailStart;
    const int oldBuiltStart = m_builtStart;
    const int oldTailStart = m_tailStart;
    const int oldTailNext = m_tailNext;
    const auto below = [first, last](int index) {
        return index <= first ? 0 : qMin(last, index - 1) - first + 1;
    };
    const int newBuiltStart = oldBuiltStart - below(oldBuiltStart);
    const int newTailStart = oldTailStart - below(oldTailStart);
    const int newTailNext = oldTailNext - below(oldTailNext + 1);

    for (int i = last; i >= first; --i) {
        TranscriptRow *row = m_rows.takeAt(i);
        m_rowHeights.removeAt(i);
        if (!row)
            continue;
        m_column->removeWidget(row);
        row->hide();
        row->deleteLater();
    }

    m_builtStart = qMax(0, newBuiltStart);
    if (tailLive) {
        m_tailStart = qMax(0, newTailStart);
        m_tailNext = newTailNext;
        if (m_tailNext < m_tailStart)
            m_tailStart = m_rows.size();
    } else {
        m_tailStart = m_rows.size();
        m_tailNext = -1;
    }

    for (int i = first; i < m_rows.size(); ++i)
        refreshRow(i);
    if (first > 0)
        refreshRow(first - 1);

    scheduleColumnHeight();
    scheduleBuild();
    refreshEmptyState();
}

void TranscriptView::updateRows(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                const QList<int> &roles)
{
    Q_UNUSED(roles)
    if (!m_model || topLeft.parent().isValid())
        return;
    const int first = qMax(0, topLeft.row());
    const int last = qMin(m_rows.size() - 1, bottomRight.row());
    for (int i = first; i <= last; ++i)
        refreshRow(i);
    if (last + 1 < m_rows.size())
        refreshRow(last + 1);
    scheduleColumnHeight();
}

void TranscriptView::refreshRow(int index)
{
    if (!m_model || index < 0 || index >= m_rows.size() || index >= m_model->rowCount())
        return;

    TranscriptRow *current = m_rows.at(index);
    if (!current)
        return;

    const RowData data = rowDataFor(m_model, m_expandedRows, index);
    if (index < m_rowHeights.size())
        m_rowHeights[index] = -1;
    if (current->rowClass() != rowClassFor(data)) {
        const int layoutIndex = layoutIndexFor(index);
        m_column->removeWidget(current);
        current->hide();
        current->deleteLater();
        TranscriptRow *row = makeRow(m_content, data);
        bindRow(row);
        row->setColumnWidth(columnWidth());
        m_rows[index] = row;
        m_column->insertWidget(layoutIndex, row);
        return;
    }
    current->setRow(data);
}

void TranscriptView::toggleExpanded(TranscriptRow *row)
{
    if (!row)
        return;
    const QString identity = rowIdentity(row->row());
    if (m_expandedRows.contains(identity))
        m_expandedRows.remove(identity);
    else
        m_expandedRows.insert(identity);
    refreshRow(row->rowIndex());
    syncColumnHeight();
    QTimer::singleShot(0, this, [this]() { if (m_followTail) applyFollow(); });
}

void TranscriptView::refreshPagingHeader()
{
    if (!m_paging)
        return;

    const bool hasEarlier = m_facade && m_facade->hasEarlier();
    if (hasEarlier) {
        m_historyStart = false;
    } else if (m_pagedEarlier) {
        m_historyStart = true;
        m_pagedEarlier = false;
    }
    m_paging->setState(hasEarlier, m_facade && m_facade->loadingEarlier(), m_historyStart);
    if (m_column)
        m_column->invalidate();
    syncColumnHeight();
    QTimer::singleShot(0, this, [this]() { if (m_followTail) applyFollow(); });
}

int TranscriptView::columnWidth() const
{
    if (!m_scroll || !m_column)
        return 600;
    const QMargins margins = m_column->contentsMargins();
    const int available = (m_content && m_content->width() > 0)
        ? m_content->width()
        : m_scroll->viewport()->width();
    return qMax(1, available - margins.left() - margins.right());
}

int TranscriptView::chromeHeight() const
{
    if (!m_column)
        return 0;
    const QMargins margins = m_column->contentsMargins();
    return margins.top() + margins.bottom() + (m_paging ? m_paging->height() : 0);
}

void TranscriptView::syncColumnHeight()
{
    m_heightQueued = false;
    if (!m_content || !m_column)
        return;

    const int count = m_rows.size();
    if (m_rowHeights.size() != count)
        m_rowHeights = QList<int>(count, -1);
    const int width = columnWidth();
    if (width != m_measuredWidth) {
        m_measuredWidth = width;
        m_rowHeights.fill(-1, count);
        for (TranscriptRow *row : m_rows) {
            if (row)
                row->setColumnWidth(width);
        }
    }

    int total = chromeHeight();
    for (int i = 0; i < count; ++i) {
        TranscriptRow *row = m_rows.at(i);
        if (!row)
            continue;
        int height = m_rowHeights.at(i);
        if (height < 0) {
            height = row->heightForWidth(width);
            m_rowHeights[i] = height;
        }
        total += height;
    }
    if (m_content->height() != total || m_content->minimumHeight() != total
            || m_content->maximumHeight() != total) {
        m_content->setFixedHeight(total);
        m_column->activate();
    }

    int laid = chromeHeight();
    bool laidOut = true;
    for (TranscriptRow *row : m_rows) {
        if (!row)
            continue;
        if (row->width() != width) {
            laidOut = false;
            break;
        }
        laid += row->height();
    }
    if (!laidOut || laid == total || laid <= 0)
        return;
    for (int i = 0; i < m_rowHeights.size(); ++i) {
        if (m_rows.at(i))
            m_rowHeights[i] = m_rows.at(i)->height();
    }
    m_content->setFixedHeight(laid);
    m_column->activate();
}

void TranscriptView::scheduleColumnHeight()
{
    if (m_heightQueued)
        return;
    m_heightQueued = true;
    QTimer::singleShot(0, this, [this]() {
        syncColumnHeight();
        if (m_followTail)
            applyFollow();
    });
}

void TranscriptView::refreshEmptyState()
{
    if (!m_empty)
        return;

    const bool empty = !m_model || m_model->rowCount() == 0;
    const bool opening = m_facade && m_facade->openingSession();
    m_empty->setVisible(empty && !opening);

    if (!empty || opening || !m_facade)
        return;

    const bool host = m_facade->transportName() == QLatin1String("dsh");
    const bool ward = m_facade->transportName() == QLatin1String("ward");
    QString text;
    if (m_facade->state() == ClientFacade::Failed) {
        text = m_facade->lastError();
        if (text.isEmpty())
            text = ward ? QStringLiteral("the ward daemon is not reachable")
                        : host ? QStringLiteral("the DSH host is not reachable")
                               : QStringLiteral("the ACP server failed to start");
    } else if (m_facade->state() == ClientFacade::Connecting
               || m_facade->state() == ClientFacade::Idle) {
        text = ward ? QStringLiteral("connecting to the ward daemon\u2026\n\nthis takes a few seconds")
                    : host ? QStringLiteral("connecting to the DSH host\u2026\n\nthis takes a few seconds")
                           : QStringLiteral("starting the ACP server\u2026\n\nthis takes a few seconds");
    } else if (m_facade->pendingSession()) {
        text = QStringLiteral("New session\n\nSend a message and it starts here.");
    } else {
        text = QStringLiteral("ready \u2014 ask something");
    }
    m_empty->setText(text);
    m_empty->raise();
}

void TranscriptView::layoutEmptyState()
{
    if (!m_empty || !m_scroll)
        return;
    const QRect viewport = m_scroll->viewport()->rect();
    m_empty->setGeometry(viewport.adjusted(40, 0, -40, 0));
    m_empty->raise();
}

bool TranscriptView::atContentEnd() const
{
    const QScrollBar *bar = m_scroll->verticalScrollBar();
    return bar->maximum() <= 0 || bar->value() >= bar->maximum() - 1;
}

bool TranscriptView::atContentStart() const
{
    const QScrollBar *bar = m_scroll->verticalScrollBar();
    return bar->value() <= bar->minimum() + 1;
}

void TranscriptView::applyFollow()
{
    if (!m_followTail || m_pinning)
        return;
    QScrollBar *bar = m_scroll->verticalScrollBar();
    if (bar->maximum() <= 0)
        return;
    if (bar->value() == bar->maximum())
        return;
    m_pinning = true;
    bar->setValue(bar->maximum());
    m_pinning = false;
    m_lastValue = bar->value();
}

bool TranscriptView::handleWheel(QWheelEvent *wheel)
{
    QScrollBar *bar = m_scroll->verticalScrollBar();
    if (bar->maximum() <= bar->minimum())
        return false;

    const int angle = wheel->angleDelta().y();
    double distance = 0.0;
    if (angle != 0) {
        const bool coarse = qAbs(angle) >= 120;
        distance = angle / 120.0 * 72.0 * (coarse ? 3.0 : 1.0);
    } else {
        distance = wheel->pixelDelta().y();
    }
    if (qFuzzyIsNull(distance))
        return false;

    if (angle != 0 && qAbs(angle) < 120) {
        m_glide->stop();
        m_glideTarget = bar->value();
        const int next = qBound(bar->minimum(),
                                qRound(bar->value() - distance), bar->maximum());
        if (next < bar->value())
            m_followTail = false;
        bar->setValue(next);
        return true;
    }

    const double base = m_glide->state() == QAbstractAnimation::Running
        ? m_glideTarget : static_cast<double>(bar->value());
    const double target = qBound(static_cast<double>(bar->minimum()),
                                 base - distance,
                                 static_cast<double>(bar->maximum()));
    if (qAbs(target - bar->value()) <= 0.5) {
        m_glide->stop();
        m_glideTarget = bar->value();
        return true;
    }
    if (target < bar->value() - 0.5)
        m_followTail = false;

    m_glideTarget = target;
    m_glide->stop();
    m_glide->setStartValue(bar->value());
    m_glide->setEndValue(target);
    m_glide->start();
    return true;
}

void TranscriptView::beginEarlierPage()
{
    QScrollBar *bar = m_scroll->verticalScrollBar();
    m_braceValue = bar->value();
    m_braceHeight = m_content->height();
    m_earlierPageArrived = false;
    m_earlierBrace = true;
}

void TranscriptView::holdEarlierPlace()
{
    if (!m_earlierBrace || !m_earlierPageArrived || m_followTail || m_pinning)
        return;
    const int grown = m_content->height() - m_braceHeight;
    if (grown <= 0)
        return;

    m_earlierBrace = false;
    m_earlierPageArrived = false;
    QScrollBar *bar = m_scroll->verticalScrollBar();
    m_pinning = true;
    bar->setValue(qMin(m_braceValue + grown, bar->maximum()));
    m_pinning = false;
    m_lastValue = bar->value();
}

void TranscriptView::maybeLoadEarlier()
{
    if (!m_facade || m_followTail || !m_earlierArmed || !atContentStart())
        return;
    if (buildPending())
        return;
    if (!m_facade->hasEarlier() || m_facade->loadingEarlier())
        return;

    m_earlierArmed = false;
    m_pagedEarlier = true;
    beginEarlierPage();
    emit pageEarlier();
    m_facade->loadEarlierHistory();
}

void TranscriptView::onScrollValueChanged(int value)
{
    QScrollBar *bar = m_scroll->verticalScrollBar();
    if (!m_pinning) {
        if (m_followTail) {
            if (value < m_lastValue - 1 && !atContentEnd())
                m_followTail = false;
        } else if (atContentEnd()) {
            m_followTail = true;
        }
    }

    if (value > bar->minimum() + 1 || m_lastValue > bar->minimum() + 1)
        m_earlierArmed = true;
    m_lastValue = value;
    maybeLoadEarlier();
}

void TranscriptView::onScrollRangeChanged(int minimum, int maximum)
{
    Q_UNUSED(minimum)
    Q_UNUSED(maximum)
    if (m_followTail)
        applyFollow();
    else if (m_earlierBrace && m_earlierPageArrived)
        holdEarlierPlace();
    maybeLoadEarlier();
}

void TranscriptView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutEmptyState();
    scheduleColumnHeight();
}

bool TranscriptView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_scroll && watched == m_scroll->viewport()) {
        if (event->type() == QEvent::Resize) {
            layoutEmptyState();
            scheduleColumnHeight();
        } else if (event->type() == QEvent::Wheel) {
            if (handleWheel(static_cast<QWheelEvent *>(event)))
                return true;
        }
    } else if (m_content && watched == m_content) {
        if (event->type() == QEvent::Resize)
            scheduleColumnHeight();
    }
    return QWidget::eventFilter(watched, event);
}
