#include "StatusStrip.h"

#include "ClientFacade.h"
#include "JobsDialog.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QLayoutItem>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QtMath>

#include <algorithm>
#include <functional>

namespace {

/// The three interactive cells share one height so the parent seat, the access
/// chip and the session id read as one object at different lengths rather than as
/// three inventions (main.qml:2940).
int cellHeight()
{
    Theme *theme = Theme::instance();
    return theme->fontData() + 2 * theme->space2();
}

// The three chip menus are built with the shell's shared helpers
// (ShellChrome.h's `Chrome::shellMenu`, `Chrome::popupMenuAt` and friends):
// a QMenu is a shell surface, and the rail's two menus use the same vocabulary.

/// A choice: one row, checked when it is the value in force.
///
/// Exclusive checkable actions rather than a painted mark, because "checked" is
/// what a menu means by "the current one" and the stock style then draws it as
/// the filled dot Fusion uses in a menu — the same mark main.qml's model and
/// effort menus draw by hand.
QAction *addChoice(QMenu *menu, QActionGroup *group, const QString &label,
                   const QString &value, bool checked, const QString &description)
{
    QAction *action = menu->addAction(Chrome::menuText(label));
    action->setCheckable(true);
    action->setData(value);
    if (!description.isEmpty())
        action->setToolTip(description);
    // Join the group before checking it, so the group's exclusivity is what
    // decides which row wears the mark.
    group->addAction(action);
    action->setChecked(checked);
    return action;
}

/// The provider order both references draw: the signed-in account first, the
/// official API second, then the catalog's own sequence — `main.qml`'s
/// `providerRank`, the web's `orderModelProviders` and Android's `providerRank`
/// are this one rule. The catalog arrives in the registry's registration order,
/// which neither trusts to put the account first.
int providerRank(const QString &provider)
{
    if (provider == QLatin1String("deepseek-account"))
        return 0;
    if (provider == QLatin1String("deepseek-official"))
        return 1;
    return 2;
}

bool localProvider(const QString &provider)
{
    const QString name = provider.trimmed().toLower();
    if (name.isEmpty())
        return false;
    if (name.contains(QLatin1String("local")) || name.contains(QLatin1String("llama"))
        || name.contains(QLatin1String("llamb")) || name.contains(QLatin1String("demesne"))
        || name.contains(QLatin1String("ollama")) || name.contains(QLatin1String("vllm"))
        || name.contains(QLatin1String("lmstudio")) || name.contains(QLatin1String("lm-studio"))
        || name.contains(QLatin1String("mlx")) || name.contains(QLatin1String("kobold"))
        || name == QLatin1String("dsh-compactor"))
        return true;
    for (const char *cloud : {"deepseek", "openai", "anthropic", "google", "gemini",
                              "mistral", "xai", "groq", "openrouter", "together",
                              "moonshot", "zhipu", "fireworks", "cohere", "cerebras",
                              "deepinfra", "perplexity", "azure", "bedrock", "vertex"}) {
        if (name.contains(QLatin1String(cloud)))
            return false;
    }
    return true;
}

QString normalisedModelName(const QString &name)
{
    QString key;
    key.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber())
            key.append(character.toLower());
    }
    return key;
}

bool modelNamesMatch(const QString &left, const QString &right)
{
    const QString a = normalisedModelName(left);
    const QString b = normalisedModelName(right);
    if (a.isEmpty() || b.isEmpty())
        return false;
    if (a.size() < 5 || b.size() < 5)
        return a == b;
    return a.startsWith(b) || b.startsWith(a);
}

int modelLoadPercent(const QVariantMap &load)
{
    if (!load.contains(QStringLiteral("progress")))
        return -1;
    double value = load.value(QStringLiteral("progress")).toDouble();
    const QString source = load.value(QStringLiteral("progressSource")).toString();
    if (source == QLatin1String("io_read_bytes") || (source.isEmpty() && value <= 1.0))
        value *= 100.0;
    return qBound(0, qRound(value), 100);
}

QString modelLoadProgressSource(const QString &source)
{
    if (source == QLatin1String("log_percent"))
        return QStringLiteral("engine log percentage");
    if (source == QLatin1String("io_read_bytes"))
        return QStringLiteral("model bytes read");
    return source;
}

int choiceRowHeight()
{
    Theme *theme = Theme::instance();
    return theme->rowHeight() + theme->space2();
}

int choiceMarkWidth()
{
    Theme *theme = Theme::instance();
    const QFontMetrics metrics(Chrome::uiFont(theme->fontUI()));
    return metrics.horizontalAdvance(QStringLiteral("\u2022")) + 2 * theme->space2();
}

int availablePopupWidth(const QRect &anchor)
{
    QScreen *screen = QGuiApplication::screenAt(anchor.topLeft());
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return QWIDGETSIZE_MAX;
    return screen->availableGeometry().width();
}

QLabel *menuHeading(QWidget *parent, const QString &text, const QColor &colour,
                    QFont::Weight weight)
{
    Theme *theme = Theme::instance();
    auto *label = Chrome::captionLabel(parent, text.toUpper());
    QFont font = label->font();
    font.setWeight(weight);
    font.setLetterSpacing(QFont::AbsoluteSpacing, theme->space1() / 2.0);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
    return label;
}

class ChoiceRow : public QWidget
{
public:
    explicit ChoiceRow(QAction *action, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_action(action)
    {
        setFont(Chrome::uiFont(Theme::instance()->fontUI()));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setToolTip(action->toolTip());
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(font());
        return QSize(2 * theme->space3() + choiceMarkWidth()
                         + metrics.horizontalAdvance(m_action->text()),
                     choiceRowHeight());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
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

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_pressed = true;
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool pressed = m_pressed;
        m_pressed = false;
        if (pressed && event->button() == Qt::LeftButton
                && rect().contains(event->position().toPoint()))
            m_action->trigger();
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        if (underMouse())
            painter.fillRect(rect(), theme->raised());

        const int mark = choiceMarkWidth();
        if (m_action->isChecked()) {
            const QRect markRect(theme->space3(), 0, mark, height());
            Chrome::paintLine(painter, markRect, font(), theme->fg(),
                              QStringLiteral("\u2022"));
        }
        const QRect labelRect(theme->space3() + mark, 0,
                              width() - 2 * theme->space3() - mark, height());
        Chrome::paintLine(painter, labelRect, font(), theme->fg(), m_action->text());
    }

private:
    QAction *m_action;
    bool m_pressed = false;
};

class HairlineDivider : public QWidget
{
public:
    explicit HairlineDivider(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedWidth(Theme::instance()->hairline());
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Theme::instance()->borderL2());
    }
};

class CellRow : public QLayout
{
public:
    enum Priority { Fixed, Flexible, Elastic, Filler };

    explicit CellRow(QWidget *parent)
        : QLayout(parent)
    {
    }

    ~CellRow() override
    {
        while (QLayoutItem *item = takeAt(0))
            delete item;
    }

    void addCell(QWidget *cell, Priority priority)
    {
        addChildWidget(cell);
        m_items.append(new QWidgetItem(cell));
        m_priorities.append(priority);
    }

    void addItem(QLayoutItem *item) override
    {
        m_items.append(item);
        m_priorities.append(Fixed);
    }

    int count() const override { return m_items.size(); }

    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }

    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_items.size())
            return nullptr;
        m_priorities.removeAt(index);
        return m_items.takeAt(index);
    }

    QSize sizeHint() const override
    {
        const QMargins margins = contentsMargins();
        int width = margins.left() + margins.right();
        int height = 0;
        int shown = 0;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i)->isEmpty())
                continue;
            const QSize hint = m_items.at(i)->sizeHint();
            width += hint.width();
            height = qMax(height, hint.height());
            ++shown;
        }
        if (shown > 1)
            width += spacing() * (shown - 1);
        return QSize(width, height + margins.top() + margins.bottom());
    }

    QSize minimumSize() const override
    {
        const QMargins margins = contentsMargins();
        int width = margins.left() + margins.right();
        int shown = 0;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i)->isEmpty())
                continue;
            ++shown;
            if (m_priorities.at(i) == Fixed || m_priorities.at(i) == Filler)
                width += m_items.at(i)->sizeHint().width();
        }
        if (shown > 1)
            width += spacing() * (shown - 1);
        return QSize(width, sizeHint().height());
    }

    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QMargins margins = contentsMargins();
        const int left = rect.left() + margins.left();
        const int top = rect.top() + margins.top();
        const int available = qMax(0, rect.width() - margins.left() - margins.right());
        const int height = qMax(0, rect.height() - margins.top() - margins.bottom());

        QList<int> shown;
        for (int i = 0; i < m_items.size(); ++i) {
            if (!m_items.at(i)->isEmpty())
                shown.append(i);
        }
        if (shown.isEmpty())
            return;

        int fixedWidth = 0;
        int flexible = -1;
        int elastic = -1;
        int filler = -1;
        for (int i : shown) {
            if (m_priorities.at(i) == Fixed || m_priorities.at(i) == Filler) {
                fixedWidth += m_items.at(i)->sizeHint().width();
                if (m_priorities.at(i) == Filler)
                    filler = i;
                continue;
            }
            if (m_priorities.at(i) == Flexible)
                flexible = i;
            else
                elastic = i;
        }

        const int gaps = spacing() * (shown.size() - 1);
        const int elasticHint = elastic >= 0 ? m_items.at(elastic)->sizeHint().width() : 0;
        int slack = available - fixedWidth - gaps - elasticHint;

        QList<int> widths(m_items.size(), 0);
        for (int i : shown) {
            if (m_priorities.at(i) == Fixed || m_priorities.at(i) == Filler)
                widths[i] = m_items.at(i)->sizeHint().width();
        }
        if (flexible >= 0) {
            widths[flexible] = qMax(0, slack);
            slack -= widths.at(flexible);
        } else if (filler >= 0 && slack > 0) {
            widths[filler] += slack;
            slack = 0;
        }
        if (elastic >= 0)
            widths[elastic] = slack < 0 ? qMax(0, elasticHint + slack) : elasticHint;

        int total = gaps;
        for (int i : shown)
            total += widths.at(i);
        int overflow = total - available;
        for (int i = shown.size() - 1; i >= 0 && overflow > 0; --i) {
            const int index = shown.at(i);
            const int give = qMin(widths.at(index), overflow);
            widths[index] -= give;
            overflow -= give;
        }

        int x = left;
        for (int i : shown) {
            m_items.at(i)->setGeometry(QRect(x, top, widths.at(i), height));
            x += widths.at(i) + spacing();
        }
    }

private:
    QList<QLayoutItem *> m_items;
    QList<Priority> m_priorities;
};

} // namespace

// A cell is a leaf: it paints one thing, its size is derived from the text it was
// given, and it reads nothing. The strip's `refresh()` owns the facade reads, so
// the eight cells cannot each hold a slightly different idea of when a derived
// label changed.

// ---- a plain line of metadata ----------------------------------------------
class StatusStrip::PlainCell : public QWidget
{
public:
    explicit PlainCell(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::uiFont(Theme::instance()->fontData()));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setValue(const QString &text, const QColor &colour, int maxWidth)
    {
        m_text = text;
        m_colour = colour;
        m_maxWidth = maxWidth;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(font());
        int width = metrics.horizontalAdvance(m_text);
        if (m_maxWidth > 0)
            width = qMin(width, m_maxWidth);
        return QSize(width, cellHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, cellHeight()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        Chrome::paintLine(painter, rect(), font(), m_colour, m_text);
    }

private:
    QString m_text;
    QColor m_colour;
    int m_maxWidth = 0;
};

// ---- a word that is a way somewhere: the parent seat ------------------------
//
// The QML seat has no border at rest: a fill on hover, `dim` text that lifts to
// `fg` under the pointer, and a tooltip that names the destination. It is quiet
// because a child session is still a session, and this is a way out rather than a
// warning (main.qml:4210).
class StatusStrip::HoverCell : public QWidget
{
public:
    explicit HoverCell(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::uiFont(Theme::instance()->fontData()));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setValue(const QString &text)
    {
        m_text = text;
        updateGeometry();
        update();
    }

    void setActivated(std::function<void()> activated) { m_activated = std::move(activated); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(font());
        return QSize(metrics.horizontalAdvance(m_text) + 2 * theme->space3(), cellHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, cellHeight()); }

protected:
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            if (m_activated)
                m_activated();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme->buttonHover());
            const int radius = theme->radius();
            painter.drawRoundedRect(rect(), radius, radius);
        }
        Chrome::paintLine(painter, rect(), font(), underMouse() ? theme->fg() : theme->dim(),
                          m_text, Qt::AlignCenter);
    }

private:
    QString m_text;
    std::function<void()> m_activated;
};

// ---- a bordered chip -------------------------------------------------------
//
// The access level, the model, the effort and the job count are chips: a hairline
// border, transparent fill, one or two runs of 12 px text. Three of them open a
// menu — the model, the effort and the access chip, the three popovers the QML
// gives this strip — and those three light under the pointer and wear the accent
// border while their list is open, as the QML's chips do. The jobs chip has no
// activation in this slice, so it keeps the quiet surface: a hover fill that opens
// nothing is worse than none.
class StatusStrip::ChipCell : public QWidget
{
public:
    explicit ChipCell(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::uiFont(Theme::instance()->fontData()));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    /// Makes the chip a trigger. Called only for the three chips with a menu;
    /// the pointer and the hover fill follow from having one.
    void setActivated(std::function<void()> activated)
    {
        m_activated = std::move(activated);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }

    /// The menu is open on this chip: the QML's second lit state, an accent
    /// border rather than a hover fill.
    void setOpen(bool open)
    {
        if (m_open == open)
            return;
        m_open = open;
        update();
    }

    /// `label` is empty for a single-run chip (model, effort, jobs); the access
    /// chip is the one two-tier chip, "access" then its value.
    void setValue(const QString &label, const QString &value, const QColor &valueColour,
                  int maxWidth)
    {
        m_label = label;
        m_value = value;
        m_valueColour = valueColour;
        m_maxWidth = maxWidth;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(font());
        int width = 2 * theme->space3();
        if (!m_label.isEmpty())
            width += metrics.horizontalAdvance(m_label) + theme->space2();
        width += metrics.horizontalAdvance(m_value);
        if (m_maxWidth > 0)
            width = qMin(width, m_maxWidth);
        return QSize(width, cellHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, cellHeight()); }

protected:
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_activated && event->button() == Qt::LeftButton
                && rect().contains(event->position().toPoint())) {
            m_activated();
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);

        const bool live = m_activated && (underMouse() || m_open);
        Chrome::paintChip(painter, rect(), live, m_open);

        // A setting lifts from its quiet tier to the text tier under the pointer
        // or while its list is open — the QML's own two-step for the effort and
        // access values. A chip without a menu keeps the tier it was given.
        const QColor colour = live ? theme->fg() : m_valueColour;
        const QRect content = rect().adjusted(theme->space3(), 0, -theme->space3(), 0);
        if (m_label.isEmpty()) {
            // One run: centred, as the model and effort labels are in QML. The
            // whole chip keeps its shape and the label elides, with the full name
            // in the tooltip.
            Chrome::paintLine(painter, content, font(), colour, m_value,
                              Qt::AlignCenter);
            return;
        }

        const QFontMetrics metrics(font());
        const int labelWidth = metrics.horizontalAdvance(m_label);
        Chrome::paintLine(painter, QRect(content.left(), 0, labelWidth, height()), font(),
                          theme->dim(), m_label);
        const QRect valueRect(content.left() + labelWidth + theme->space2(), 0,
                              content.width() - labelWidth - theme->space2(), height());
        Chrome::paintLine(painter, valueRect, font(), colour, m_value,
                          Qt::AlignLeft | Qt::AlignVCenter);
    }

private:
    QString m_label;
    QString m_value;
    QColor m_valueColour;
    int m_maxWidth = 0;
    bool m_open = false;
    std::function<void()> m_activated;
};

// ---- the session id --------------------------------------------------------
//
// The only flexible cell: it takes the strip's slack and gives it back first when
// the window narrows, because the id is the answer the bar exists to give. Mono,
// bold, elided in the middle (both ends of a uuid are fixed), with a copy on click
// — the accent fill is the one loud confirmation this 24 px strip can give.
class StatusStrip::SessionIdCell : public QWidget
{
public:
    explicit SessionIdCell(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::monoFont(Theme::instance()->fontData(), true));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("Click to copy this session id"));
    }

    void setId(const QString &id)
    {
        m_id = id;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(QFontMetrics(font()).horizontalAdvance(m_id), cellHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, cellHeight()); }

protected:
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !m_id.isEmpty()) {
            QGuiApplication::clipboard()->setText(m_id);
            m_copied = true;
            setToolTip(QStringLiteral("Copied"));
            update();
            QTimer::singleShot(1600, this, [this]() {
                m_copied = false;
                setToolTip(QStringLiteral("Click to copy this session id"));
                update();
            });
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QFontMetrics metrics(font());
        const int textWidth = qMin(metrics.horizontalAdvance(m_id), width());
        const QRect hover(0, 0, textWidth, height());

        if (m_copied || underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_copied ? theme->accent() : theme->buttonHover());
            const int radius = theme->radius();
            painter.drawRoundedRect(hover, radius, radius);
        }

        Chrome::paintLine(painter, rect(), font(), theme->fg(), m_id,
                          Qt::AlignLeft | Qt::AlignVCenter, true);
    }

private:
    QString m_id;
    bool m_copied = false;
};

// ---- the context ring and its two counts ------------------------------------
//
// The web's ring geometry, from the token table: a 14 px square, a 2 px stroke
// with a round cap, starting at twelve o'clock and sweeping the ROUNDED
// percentage, so the arc and the tooltip's number can never disagree
// (main.qml:4040). One fill colour at every fraction — the web has no
// thresholds — and the fill is `caption`, the tier of the count beside it.
class StatusStrip::ContextCell : public QWidget
{
public:
    explicit ContextCell(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::uiFont(Theme::instance()->fontData()));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setCounts(int used, int size, int percent)
    {
        m_text = Chrome::compactCount(used) + QStringLiteral(" / ") + Chrome::compactCount(size);
        m_percent = percent;
        setToolTip(Chrome::compactCount(used) + QStringLiteral(" / ")
                   + Chrome::compactCount(size) + QStringLiteral(" tokens used · ")
                   + QString::number(percent) + QStringLiteral("%"));
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(font());
        return QSize(theme->contextRing() + theme->space3()
                         + metrics.horizontalAdvance(m_text),
                     cellHeight());
    }

    QSize minimumSizeHint() const override { return QSize(0, cellHeight()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int ring = theme->contextRing();
        const qreal stroke = theme->contextRingStroke();
        const qreal radius = (ring - stroke - theme->hairline()) / 2.0;
        const QPointF centre(ring / 2.0, height() / 2.0);
        const QRectF circle(centre.x() - radius, centre.y() - radius,
                           2.0 * radius, 2.0 * radius);

        QPen pen(theme->contextRingTrack(), stroke);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(circle);

        // Before the first token there is nothing to sweep; the track alone is the
        // honest reading, not a dot.
        if (m_percent > 0) {
            pen.setColor(theme->caption());
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.drawArc(circle, 90 * 16, -qRound(360.0 * m_percent / 100.0) * 16);
        }

        const QRect textRect(ring + theme->space3(), 0,
                             width() - ring - theme->space3(), height());
        Chrome::paintLine(painter, textRect, font(), theme->caption(), m_text);
    }

private:
    QString m_text;
    int m_percent = 0;
};

// ---- the strip -------------------------------------------------------------

int StatusStrip::stripHeight()
{
    Theme *theme = Theme::instance();
    return theme->fontData() + 2 * theme->space3();   // 12 + 12 = 24
}

StatusStrip::StatusStrip(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();
    setObjectName(QStringLiteral("statusStrip"));
    setFixedHeight(stripHeight());

    auto *row = new CellRow(this);
    // One line of 20 px cells in 24 px: the vertical inset is (24 - 20) / 2 = 2,
    // which is space1. The horizontal insets and the spacing are the QML strip's
    // (main.qml:4202, 3567).
    row->setContentsMargins(theme->space5(), theme->space1(), theme->space5(), theme->space1());
    row->setSpacing(theme->space4());

    m_parent = new HoverCell(this);
    m_parent->setValue(QStringLiteral("\u2191  Parent"));
    m_parent->setActivated([this]() { m_facade->followParentSession(); });
    row->addCell(m_parent, CellRow::Fixed);

    m_status = new PlainCell(this);
    row->addCell(m_status, CellRow::Filler);

    m_access = new ChipCell(this);
    m_access->setObjectName(QStringLiteral("accessChip"));
    row->addCell(m_access, CellRow::Fixed);

    m_session = new SessionIdCell(this);
    m_session->setObjectName(QStringLiteral("sessionIdChip"));
    row->addCell(m_session, CellRow::Flexible);

    m_load = new ChipCell(this);
    m_load->setObjectName(QStringLiteral("modelLoadChip"));
    row->addCell(m_load, CellRow::Fixed);

    m_model = new ChipCell(this);
    m_model->setObjectName(QStringLiteral("modelChip"));
    row->addCell(m_model, CellRow::Elastic);

    m_effort = new ChipCell(this);
    m_effort->setObjectName(QStringLiteral("effortChip"));
    row->addCell(m_effort, CellRow::Fixed);

    m_context = new ContextCell(this);
    m_context->setObjectName(QStringLiteral("contextChip"));
    row->addCell(m_context, CellRow::Fixed);

    m_jobs = new ChipCell(this);
    m_jobs->setObjectName(QStringLiteral("jobsChip"));
    row->addCell(m_jobs, CellRow::Fixed);

    // The three chips with a popover in the QML get one here. The context cell
    // is a ring and a tooltip and the jobs chip's roster is a later slice; both
    // stay inert rather than opening an empty menu.
    m_access->setActivated([this]() { openAccessMenu(); });
    m_jobs->setActivated([this]() {
        auto *dialog = new JobsDialog(m_facade, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    if (QObject *transport = facade->transport()) {
        if (transport->metaObject()->indexOfSignal("dangerousPresetRequested(QString,QString)") >= 0)
            connect(transport, SIGNAL(dangerousPresetRequested(QString,QString)), this,
                    SLOT(onDangerousPresetRequested(QString,QString)));
    }
    m_model->setActivated([this]() { openModelMenu(); });
    m_load->setActivated([this]() { openEstateMenu(); });
    m_effort->setActivated([this]() { openEffortMenu(); });

    // One recompute for every fact the strip draws. A derived label (the model's
    // name, the access level's name) can move on any of these notifies, and the
    // cost is eight short strings.
    const auto relayout = [this]() { refresh(); };
    QObject::connect(facade, &ClientFacade::statusChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::sessionIdChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::parentSeatChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::permissionPresetChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::permissionPresetsChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::accessNoticeChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::configOptionsChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::modelNoticeChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::modelLoadsChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::contextChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::contextSizeChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::jobsChanged, this, relayout);
    QObject::connect(facade, &ClientFacade::liveJobCountChanged, this, relayout);

    QObject::connect(facade, &ClientFacade::permissionPresetsChanged, this, [this]() {
        if (m_accessMenu != nullptr && m_accessMenu->isVisible())
            fillAccessMenu();
    });

    QObject::connect(facade, &ClientFacade::configOptionsChanged, this, [this]() {
        if (m_modelMenu != nullptr && m_modelMenu->isVisible())
            fillModelMenu();
    });

    QObject::connect(facade, &ClientFacade::stateChanged, this, [facade]() {
        if (facade->state() == ClientFacade::Ready)
            facade->refreshPermissionPresets();
    });

    refresh();
}

void StatusStrip::refresh()
{
    Theme *theme = Theme::instance();

    // ---- leading: parent, status, access, id --------------------------------
    const QString parentId = m_facade->parentSessionId();
    m_parent->setVisible(!parentId.isEmpty());
    m_parent->setToolTip(QStringLiteral("Open parent session: ")
                         + (m_facade->parentSessionTitle().isEmpty()
                                ? QStringLiteral("parent")
                                : m_facade->parentSessionTitle()));

    // A capped status: a long sentence ("steering · delivered at the next step
    // boundary") must not push the id out of the strip, and the id is the answer
    // that has to survive.
    m_status->setValue(m_facade->status(), theme->dim(), 16 * theme->space6());

    const QString access = accessLevelName();
    m_access->setVisible(!access.isEmpty());
    m_access->setValue(QStringLiteral("access"), access,
                       accessDangerous() ? theme->danger() : theme->dim(), 0);
    m_access->setToolTip(!m_facade->accessNotice().isEmpty()
                             ? m_facade->accessNotice()
                             : (!accessDescription().isEmpty()
                                    ? accessDescription()
                                    : QStringLiteral("Permission preset for this session")));

    const QString id = m_facade->sessionId();
    m_session->setVisible(!id.isEmpty());
    m_session->setId(id);

    // ---- trailing: model, effort, context, jobs ----------------------------
    const QString model = modelName();
    m_model->setVisible(!model.isEmpty());
    m_model->setValue(QString(), model, theme->fg(), 12 * theme->space6());
    m_model->setToolTip(!m_facade->modelNotice().isEmpty()
                            ? m_facade->modelNotice()
                            : QStringLiteral("Model for this session: ") + model);

    const QVariantMap activeLoad = activeModelLoad();
    refreshModelLoad(activeLoad);

    const QString effort = effortLabel();
    m_effort->setVisible(!effort.isEmpty());
    m_effort->setValue(QString(), effort, theme->dim(), 10 * theme->space6());
    const QString effortDescription = choiceDescription(effortOption());
    m_effort->setToolTip(!m_facade->modelNotice().isEmpty()
                             ? m_facade->modelNotice()
                             : QStringLiteral("Reasoning effort for this model: ") + effort
                               + (effortDescription.isEmpty()
                                      ? QString()
                                      : QStringLiteral("\n") + effortDescription));

    const bool hasContext = m_facade->contextSize() > 0;
    m_context->setVisible(hasContext);
    if (hasContext)
        m_context->setCounts(m_facade->contextUsed(), m_facade->contextSize(),
                            contextPercent());

    const int jobCount = m_facade->jobs().size();
    m_jobs->setVisible(jobCount > 0);
    m_jobs->setValue(QString(), jobText(), jobsLive() ? theme->fg() : theme->caption(), 0);

    // What the strip is showing, in the log. This is evidence, not decoration:
    // the chrome is one 24 px row and a run cannot be screenshotted from here, so
    // the values it resolved from the facade are written down when they change.
    const QString loadWord = modelPhase(activeLoad);
    const QString shown =
        QStringLiteral("parent=%1 status=\"%2\" access=%3 session=%4 model=%5"
                       " effort=%6 context=%7/%8 jobs=%9 load=%10")
            .arg(parentId.isEmpty() ? QStringLiteral("-") : parentId)
            .arg(m_facade->status())
            .arg(access.isEmpty() ? QStringLiteral("-") : access)
            .arg(id.isEmpty() ? QStringLiteral("-") : id)
            .arg(model.isEmpty() ? QStringLiteral("-") : model)
            .arg(effort.isEmpty() ? QStringLiteral("-") : effort)
            .arg(m_facade->contextUsed())
            .arg(m_facade->contextSize())
            .arg(jobCount > 0 ? jobText() : QStringLiteral("-"))
            .arg(loadWord.isEmpty() ? QStringLiteral("-") : loadWord);
    if (shown != m_loggedLine) {
        m_loggedLine = shown;
        qInfo().nospace() << "status strip: " << shown;
    }
}

// ---- the three chip menus --------------------------------------------------

void StatusStrip::openModelMenu()
{
    m_facade->refreshModelCatalog();

    m_modelAnchor = QRect(m_model->mapToGlobal(QPoint(0, 0)), m_model->size());

    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    menu->installEventFilter(this);
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);

    m_modelMenu = menu;
    m_modelGroup = group;

    const int rows = fillModelMenu();

    qInfo().nospace() << "status menu: model open (" << rows << " choices)";

    connect(menu, &QMenu::aboutToShow, m_model, [this]() { m_model->setOpen(true); });
    connect(menu, &QMenu::aboutToHide, m_model, [this]() { m_model->setOpen(false); });
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        m_modelMenu = nullptr;
        m_modelGroup = nullptr;
    });

    Chrome::popupMenuAt(m_modelAnchor, menu, true);

    QPointer<StatusStrip> self(this);
    QTimer::singleShot(0, menu, [self, menu]() {
        if (self.isNull() || !menu->isVisible() || self->m_modelMenu != menu)
            return;
        self->fitModelBodies();
    });
}

int StatusStrip::fillModelMenu()
{
    QMenu *menu = m_modelMenu.data();
    QActionGroup *group = m_modelGroup.data();
    if (menu == nullptr || group == nullptr)
        return 0;

    const bool live = menu->isVisible();
    const int localScroll =
        live && m_localColumn.body ? m_localColumn.body->verticalScrollBar()->value() : 0;
    const int cloudScroll =
        live && m_cloudColumn.body ? m_cloudColumn.body->verticalScrollBar()->value() : 0;

    const QList<QAction *> previous = group->actions();
    for (QAction *action : previous) {
        menu->removeAction(action);
        group->removeAction(action);
        delete action;
    }
    menu->clear();

    const QVariantMap option = modelOption();
    m_modelConfigId = option.value(QStringLiteral("id")).toString();
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    const QVariantList choices = option.value(QStringLiteral("choices")).toList();

    struct Section
    {
        QString provider;
        QString caption;
        QVariantList choices;
    };
    QList<Section> sections;
    for (const QVariant &entry : choices) {
        const QVariantMap choice = entry.toMap();
        const QString provider = choice.value(QStringLiteral("provider")).toString();
        qsizetype at = -1;
        for (qsizetype i = 0; i < sections.size(); ++i) {
            if (sections.at(i).provider == provider) {
                at = i;
                break;
            }
        }
        if (at < 0) {
            at = sections.size();
            sections.append(Section{provider,
                                    choice.value(QStringLiteral("group")).toString(),
                                    {}});
        }
        sections[at].choices.append(choice);
    }

    std::stable_sort(sections.begin(), sections.end(),
                     [](const Section &left, const Section &right) {
                         return providerRank(left.provider) < providerRank(right.provider);
                     });

    QList<Section> local;
    QList<Section> cloud;
    bool twoColumns = !sections.isEmpty();
    for (const Section &section : sections) {
        if (section.provider.isEmpty()) {
            twoColumns = false;
            break;
        }
        if (localProvider(section.provider))
            local.append(section);
        else
            cloud.append(section);
    }
    if (local.isEmpty() || cloud.isEmpty())
        twoColumns = false;

    int rows = 0;
    bool built = false;

    if (twoColumns) {
        Theme *theme = Theme::instance();

        auto *columns = new QWidget;
        columns->setFocusPolicy(Qt::NoFocus);
        auto *columnLayout = new QHBoxLayout(columns);
        columnLayout->setContentsMargins(0, theme->space1(), 0, theme->space1());
        columnLayout->setSpacing(theme->space3());

        auto addColumn = [&](const QString &caption,
                             const QList<Section> &columnSections) {
            BodyColumn column;
            column.widget = new QWidget(columns);
            column.widget->setFocusPolicy(Qt::NoFocus);
            auto *layout = new QVBoxLayout(column.widget);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);

            auto *heading = menuHeading(column.widget, caption, theme->dim(), QFont::Medium);
            layout->addWidget(heading);
            column.captionHeight = heading->sizeHint().height();
            column.contentWidth = heading->sizeHint().width();

            column.body = new QScrollArea(column.widget);
            column.body->setFrameShape(QFrame::NoFrame);
            column.body->setWidgetResizable(false);
            column.body->setFocusPolicy(Qt::NoFocus);
            column.body->viewport()->setFocusPolicy(Qt::NoFocus);
            column.body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            column.body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            Chrome::styleScrollBar(column.body->verticalScrollBar());
            QPalette palette = column.body->palette();
            palette.setColor(QPalette::Base, theme->panel());
            palette.setColor(QPalette::Window, theme->panel());
            column.body->setPalette(palette);
            column.body->setBackgroundRole(QPalette::Base);
            column.body->viewport()->setAutoFillBackground(true);

            auto *content = new QWidget;
            content->setFocusPolicy(Qt::NoFocus);
            column.content = content;
            auto *contentLayout = new QVBoxLayout(content);
            contentLayout->setContentsMargins(0, 0, 0, 0);
            contentLayout->setSpacing(0);

            for (const Section &section : columnSections) {
                if (columnSections.size() > 1 && !section.caption.isEmpty()) {
                    auto *sub = menuHeading(content, section.caption, theme->caption(),
                                            QFont::Normal);
                    contentLayout->addWidget(sub);
                    column.contentWidth = qMax(column.contentWidth, sub->sizeHint().width());
                }
                for (const QVariant &entry : section.choices) {
                    const QVariantMap choice = entry.toMap();
                    const QString value = choice.value(QStringLiteral("value")).toString();
                    auto *action = new QAction(
                        choice.value(QStringLiteral("label")).toString(), menu);
                    action->setCheckable(true);
                    action->setData(value);
                    const QString description =
                        choice.value(QStringLiteral("description")).toString();
                    if (!description.isEmpty())
                        action->setToolTip(description);
                    group->addAction(action);
                    action->setChecked(value == current);
                    connect(action, &QAction::triggered, this, [this, action, menu]() {
                        const QString chosen = action->data().toString();
                        menu->close();
                        chooseModel(chosen);
                    });
                    auto *row = new ChoiceRow(action, content);
                    contentLayout->addWidget(row);
                    column.contentWidth = qMax(column.contentWidth, row->sizeHint().width());
                    ++rows;
                }
            }

            column.body->setWidget(content);
            column.contentHeight = contentLayout->sizeHint().height();
            column.scrollBarWidth = column.body->verticalScrollBar()->sizeHint().width();
            content->setFixedSize(column.contentWidth, column.contentHeight);
            layout->addWidget(column.body);
            return column;
        };

        BodyColumn localColumn = addColumn(QStringLiteral("Local"), local);
        columnLayout->addWidget(localColumn.widget, 0, Qt::AlignTop);
        columnLayout->addWidget(new HairlineDivider(columns));
        BodyColumn cloudColumn = addColumn(QStringLiteral("Cloud"), cloud);
        columnLayout->addWidget(cloudColumn.widget, 0, Qt::AlignTop);

        auto *widgetAction = new QWidgetAction(menu);
        widgetAction->setDefaultWidget(columns);
        menu->addAction(widgetAction);

        auto menuHint = [&]() {
            menu->removeAction(widgetAction);
            menu->addAction(widgetAction);
            return menu->sizeHint();
        };

        const QSize natural = menuHint();
        const int chrome = qMax(0, natural.height() - columns->sizeHint().height());
        const Chrome::MenuRoom room = Chrome::popupMenuRoom(m_modelAnchor, natural.height());
        const int safeRoom = qMax(0, room.room - theme->space3());
        const int columnCap = qMax(0, safeRoom - chrome - 2 * theme->space1());

        auto fitColumn = [&](BodyColumn *column) {
            column->bodyHeight = qMin(column->contentHeight,
                                      qMax(0, columnCap - column->captionHeight));
            column->scrolls = column->contentHeight > column->bodyHeight;
            column->body->setVerticalScrollBarPolicy(
                column->scrolls ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
            column->body->setFixedHeight(column->bodyHeight);
            column->content->setFixedSize(column->contentWidth, column->contentHeight);
            column->widget->setFixedWidth(column->contentWidth
                                          + (column->scrolls ? column->scrollBarWidth : 0));
            QScrollBar *bar = column->body->verticalScrollBar();
            bar->setSingleStep(qMax(1, choiceRowHeight()));
            bar->setPageStep(qMax(1, column->bodyHeight));
            bar->setRange(0, column->contentHeight - column->bodyHeight);
        };

        fitColumn(&localColumn);
        fitColumn(&cloudColumn);
        columns->setFixedWidth(columnLayout->sizeHint().width());

        QSize finalHint = menuHint();
        for (int pass = 0; pass < 3 && finalHint.height() > room.room; ++pass) {
            BodyColumn *tallest = &localColumn;
            const int localHeight = localColumn.captionHeight + localColumn.body->height();
            const int cloudHeight = cloudColumn.captionHeight + cloudColumn.body->height();
            if (cloudHeight > localHeight)
                tallest = &cloudColumn;
            const int excess = finalHint.height() - room.room;
            const int body = qMax(0, tallest->body->height() - excess);
            if (body == tallest->body->height())
                break;
            tallest->bodyHeight = body;
            tallest->scrolls = tallest->contentHeight > body;
            tallest->body->setVerticalScrollBarPolicy(
                tallest->scrolls ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
            tallest->body->setFixedHeight(body);
            tallest->body->verticalScrollBar()->setRange(0, tallest->contentHeight - body);
            tallest->widget->setFixedWidth(tallest->contentWidth
                                           + (tallest->scrolls
                                                  ? tallest->scrollBarWidth : 0));
            columns->setFixedWidth(columnLayout->sizeHint().width());
            finalHint = menuHint();
        }

        localColumn.body->verticalScrollBar()->setValue(
            live && localColumn.scrolls ? localScroll : 0);
        cloudColumn.body->verticalScrollBar()->setValue(
            live && cloudColumn.scrolls ? cloudScroll : 0);

        if (finalHint.width() <= availablePopupWidth(m_modelAnchor)) {
            built = true;
            m_modelColumns = columns;
            m_localColumn = localColumn;
            m_cloudColumn = cloudColumn;
            m_modelChrome = chrome;
            columns->installEventFilter(this);
            localColumn.widget->installEventFilter(this);
            cloudColumn.widget->installEventFilter(this);
            localColumn.body->viewport()->installEventFilter(this);
            cloudColumn.body->viewport()->installEventFilter(this);
            if (!live) {
                qInfo().nospace()
                    << "status menu: model room side="
                    << (room.aboveAnchor ? "above" : "below")
                    << " below=" << room.below << " above=" << room.above
                    << " room=" << room.room << " natural=" << natural.height()
                    << " menu=" << finalHint.width() << "x" << finalHint.height()
                    << " body=" << localColumn.bodyHeight << "/" << cloudColumn.bodyHeight
                    << " content=" << localColumn.contentHeight << "/"
                    << cloudColumn.contentHeight
                    << " scroll=" << localColumn.scrolls << "/" << cloudColumn.scrolls;
            }
        } else {
            for (QAction *action : group->actions()) {
                menu->removeAction(action);
                group->removeAction(action);
                delete action;
            }
            menu->clear();
            m_modelColumns = nullptr;
            m_localColumn = BodyColumn();
            m_cloudColumn = BodyColumn();
            rows = 0;
        }
    }

    if (!built) {
        for (const Section &section : sections) {
            if (!section.provider.isEmpty() && !section.caption.isEmpty())
                Chrome::addCaption(menu, section.caption);
            for (const QVariant &entry : section.choices) {
                const QVariantMap choice = entry.toMap();
                const QString value = choice.value(QStringLiteral("value")).toString();
                auto *action = addChoice(menu, group,
                                         choice.value(QStringLiteral("label")).toString(),
                                         value, value == current,
                                         choice.value(QStringLiteral("description")).toString());
                connect(action, &QAction::triggered, this, [this, action]() {
                    chooseModel(action->data().toString());
                });
                ++rows;
            }
        }
        if (rows == 0)
            Chrome::addEmptyRow(menu, QStringLiteral("No model catalog on this transport"));
        m_modelColumns = nullptr;
        m_localColumn = BodyColumn();
        m_cloudColumn = BodyColumn();
    }

    if (live) {
        QPointer<StatusStrip> self(this);
        QTimer::singleShot(0, menu, [self, menu]() {
            if (self.isNull() || !menu->isVisible() || self->m_modelMenu != menu)
                return;
            self->fitModelBodies();
        });
        qInfo().nospace() << "status menu: model filled while open (" << rows
                          << " choices)";
    }
    return rows;
}

void StatusStrip::fitModelBodies()
{
    QMenu *menu = m_modelMenu.data();
    QWidget *columns = m_modelColumns.data();
    if (menu == nullptr || columns == nullptr || !menu->isVisible())
        return;

    Theme *theme = Theme::instance();
    const int menuHeight = menu->height();
    if (menuHeight <= 0)
        return;

    const int chrome = m_modelChrome;
    const int inner = qMax(0, menuHeight - chrome);

    const int previousColumnsWidth = columns->width();
    BodyColumn *fitted[] = {&m_localColumn, &m_cloudColumn};
    int tallest = 0;
    for (BodyColumn *column : fitted) {
        if (column->widget.isNull() || column->content.isNull() || column->body.isNull())
            continue;
        const int cap = qMax(0, inner - 2 * theme->space1() - column->captionHeight);
        column->bodyHeight = qMin(column->contentHeight, cap);
        column->scrolls = column->contentHeight > column->bodyHeight;
        column->body->setVerticalScrollBarPolicy(column->scrolls ? Qt::ScrollBarAlwaysOn
                                                                 : Qt::ScrollBarAlwaysOff);
        column->body->setFixedHeight(column->bodyHeight);
        column->content->setFixedSize(column->contentWidth, column->contentHeight);
        column->widget->setFixedWidth(column->contentWidth
                                      + (column->scrolls ? column->scrollBarWidth : 0));
        QScrollBar *bar = column->body->verticalScrollBar();
        bar->setSingleStep(qMax(1, choiceRowHeight()));
        bar->setPageStep(qMax(1, column->bodyHeight));
        bar->setRange(0, qMax(0, column->contentHeight - column->bodyHeight));
        tallest = qMax(tallest, column->captionHeight + column->bodyHeight
                                    + 2 * theme->space1());
    }

    if (columns->layout() != nullptr) {
        columns->layout()->activate();
        const int columnsWidth = columns->layout()->sizeHint().width();
        if (columnsWidth > 0) {
            columns->setFixedWidth(columnsWidth);
            const int wantedWidth = menu->width() + columnsWidth - previousColumnsWidth;
            QScreen *screen = QGuiApplication::screenAt(menu->mapToGlobal(QPoint(0, 0)));
            if (screen == nullptr)
                screen = QGuiApplication::primaryScreen();
            const QRect available = screen != nullptr ? screen->availableGeometry() : QRect();
            if (wantedWidth > menu->width() && !available.isNull()
                && menu->x() + wantedWidth - 1 <= available.right()) {
                menu->resize(wantedWidth, menu->height());
            }
        }
    }

    const int target = chrome + tallest;
    if (tallest > 0 && target > 0 && target < menu->height())
        menu->resize(menu->width(), target);

    const int finalInner = qMax(0, menu->height() - chrome);
    for (BodyColumn *column : fitted) {
        if (column->body.isNull())
            continue;
        if (column->body->height() > finalInner) {
            qWarning().nospace() << "status menu: model body exceeds menu body="
                                 << column->body->height() << " menu=" << menu->height()
                                 << " chrome=" << chrome;
        }
    }

    QScreen *screen = QGuiApplication::screenAt(menu->mapToGlobal(QPoint(0, 0)));
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    auto describe = [](const BodyColumn &column) {
        if (column.body.isNull())
            return QStringLiteral("body=-");
        const QScrollBar *bar = column.body->verticalScrollBar();
        return QStringLiteral("body=%1x%2 viewport=%3 max=%4 vis=%5")
            .arg(column.body->width())
            .arg(column.body->height())
            .arg(column.body->viewport()->height())
            .arg(bar->maximum())
            .arg(bar->isVisible() ? 1 : 0);
    };
    qInfo().nospace() << "status menu: model shown menu=" << menu->geometry()
                      << " menuH=" << menu->height() << " chrome=" << chrome
                      << " screen="
                      << (screen != nullptr ? screen->availableGeometry() : QRect())
                      << " local[" << describe(m_localColumn) << "] cloud["
                      << describe(m_cloudColumn) << "]";
}

bool StatusStrip::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::Wheel || m_modelMenu == nullptr
        || !m_modelMenu->isVisible())
        return QWidget::eventFilter(watched, event);
    auto *wheel = static_cast<QWheelEvent *>(event);
    const QPoint global = wheel->globalPosition().toPoint();

    QScrollArea *bodies[] = {m_localColumn.body.data(), m_cloudColumn.body.data()};
    QScrollArea *target = nullptr;
    for (QScrollArea *body : bodies) {
        if (body == nullptr || !body->isVisible())
            continue;
        QWidget *column = body->parentWidget();
        if (column != nullptr && column->isVisible()
            && column->rect().contains(column->mapFromGlobal(global))) {
            target = body;
            break;
        }
    }
    if (target == nullptr && m_modelMenu->rect().contains(
                                 m_modelMenu->mapFromGlobal(global))) {
        QWidget *local = m_localColumn.body != nullptr
                             ? m_localColumn.body->parentWidget() : nullptr;
        QWidget *cloud = m_cloudColumn.body != nullptr
                             ? m_cloudColumn.body->parentWidget() : nullptr;
        if (local != nullptr && cloud != nullptr) {
            const QRect localRect(local->mapToGlobal(QPoint(0, 0)), local->size());
            const QRect cloudRect(cloud->mapToGlobal(QPoint(0, 0)), cloud->size());
            target = qAbs(global.x() - localRect.center().x())
                             <= qAbs(global.x() - cloudRect.center().x())
                         ? m_localColumn.body.data()
                         : m_cloudColumn.body.data();
        } else {
            target = m_localColumn.body != nullptr ? m_localColumn.body.data()
                                                   : m_cloudColumn.body.data();
        }
    }
    if (target == nullptr)
        return QWidget::eventFilter(watched, event);

    QScrollBar *bar = target->verticalScrollBar();
    if (bar == nullptr || bar->maximum() <= 0)
        return QWidget::eventFilter(watched, event);

    const int angle = wheel->angleDelta().y();
    const int pixels = wheel->pixelDelta().y();
    if (angle == 0 && pixels == 0)
        return QWidget::eventFilter(watched, event);

    const int step = qMax(1, bar->singleStep());
    int delta = pixels != 0 ? pixels : qRound(angle / 120.0 * step);
    if (delta == 0)
        delta = angle > 0 ? step : -step;
    bar->setValue(bar->value() - delta);
    return true;
}

void StatusStrip::chooseModel(const QString &value)
{
    if (m_modelConfigId.isEmpty())
        return;
    qInfo().nospace() << "status menu: model choose id=" << m_modelConfigId
                      << " value=" << value;
    m_facade->setConfigOption(m_modelConfigId, value);
}

void StatusStrip::openEffortMenu()
{
    const QVariantMap option = effortOption();
    const QString configId = option.value(QStringLiteral("id")).toString();
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    const QVariantList choices = option.value(QStringLiteral("choices")).toList();

    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);

    // The word the trigger does not spend width on, as the QML's caption: the
    // chip says "high", the list says what "high" is a level of.
    Chrome::addCaption(menu, QStringLiteral("Reasoning effort"));

    int rows = 0;
    for (const QVariant &entry : choices) {
        const QVariantMap choice = entry.toMap();
        const QString value = choice.value(QStringLiteral("value")).toString();
        addChoice(menu, group, choice.value(QStringLiteral("label")).toString(), value,
                  value == current,
                  choice.value(QStringLiteral("description")).toString());
        ++rows;
    }
    if (rows == 0)
        Chrome::addEmptyRow(menu, QStringLiteral("No reasoning efforts on this transport"));

    qInfo().nospace() << "status menu: effort open (" << rows << " levels)";

    connect(menu, &QMenu::triggered, this, [this, configId](QAction *action) {
        const QString value = action->data().toString();
        if (configId.isEmpty())
            return;
        qInfo().nospace() << "status menu: effort choose id=" << configId
                          << " value=" << value;
        m_facade->setConfigOption(configId, value);
    });
    connect(menu, &QMenu::aboutToShow, m_effort, [this]() { m_effort->setOpen(true); });
    connect(menu, &QMenu::aboutToHide, m_effort, [this]() { m_effort->setOpen(false); });

    Chrome::popupMenuAt(QRect(m_effort->mapToGlobal(QPoint(0, 0)), m_effort->size()), menu, true);
}

void StatusStrip::openAccessMenu()
{
    m_facade->refreshPermissionPresets();

    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);

    m_accessMenu = menu;
    m_accessGroup = group;

    const int rows = fillAccessMenu();

    qInfo().nospace() << "status menu: access open (" << rows << " presets)";

    connect(menu, &QMenu::triggered, this, [this](QAction *action) {
        const QString value = action->data().toString();
        qInfo().nospace() << "status menu: access choose preset=" << value;
        m_facade->setPermissionPreset(value);
    });
    connect(menu, &QMenu::aboutToShow, m_access, [this]() { m_access->setOpen(true); });
    connect(menu, &QMenu::aboutToHide, m_access, [this]() { m_access->setOpen(false); });
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        m_accessMenu = nullptr;
        m_accessGroup = nullptr;
    });

    Chrome::popupMenuAt(QRect(m_access->mapToGlobal(QPoint(0, 0)), m_access->size()), menu, true);
}

int StatusStrip::fillAccessMenu()
{
    QMenu *menu = m_accessMenu.data();
    QActionGroup *group = m_accessGroup.data();
    if (menu == nullptr || group == nullptr)
        return 0;

    const bool live = menu->isVisible();
    const QVariantList presets = m_facade->permissionPresets();
    const QString current = m_facade->permissionPreset();

    const QList<QAction *> previous = group->actions();
    for (QAction *action : previous)
        group->removeAction(action);
    menu->clear();

    int rows = 0;
    for (const QVariant &entry : presets) {
        const QVariantMap preset = entry.toMap();
        const QString value = preset.value(QStringLiteral("value")).toString();
        addChoice(menu, group, preset.value(QStringLiteral("name")).toString(), value,
                  value == current,
                  preset.value(QStringLiteral("description")).toString());
        ++rows;
    }
    if (rows == 0) {
        // The QML's two empty sentences: a level in force with no catalog yet is
        // a list being read, not a transport without presets.
        Chrome::addEmptyRow(menu, current.isEmpty()
                              ? QStringLiteral("No presets on this transport")
                              : QStringLiteral("Reading the preset list\u2026"));
    }

    if (live) {
        menu->adjustSize();
        qInfo().nospace() << "status menu: access filled while open (" << rows
                          << " presets)";
    }
    return rows;
}

// ---- the derived values ----------------------------------------------------

QVariantMap StatusStrip::modelOption() const
{
    const QVariantList options = m_facade->configOptions();
    for (const QVariant &entry : options) {
        const QVariantMap option = entry.toMap();
        if (option.value(QStringLiteral("id")).toString() == QLatin1String("model")
                || option.value(QStringLiteral("category")).toString() == QLatin1String("model"))
            return option;
    }
    return {};
}

QVariantMap StatusStrip::effortOption() const
{
    // Reasoning effort is a separate catalog entry, and it exists only when the
    // selected model declares efforts — so the chip is absent, not empty, on a
    // model that has none (main.qml:3138).
    const QVariantList options = m_facade->configOptions();
    for (const QVariant &entry : options) {
        const QVariantMap option = entry.toMap();
        if (option.value(QStringLiteral("id")).toString() == QLatin1String("reasoning_effort")
                || option.value(QStringLiteral("category")).toString()
                       == QLatin1String("thought_level"))
            return option;
    }
    return {};
}

QString StatusStrip::choiceLabel(const QVariantMap &option) const
{
    if (option.isEmpty())
        return {};
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    const QVariantList choices = option.value(QStringLiteral("choices")).toList();
    for (const QVariant &entry : choices) {
        const QVariantMap choice = entry.toMap();
        if (choice.value(QStringLiteral("value")).toString() == current)
            return choice.value(QStringLiteral("label")).toString();
    }
    return {};
}

QString StatusStrip::choiceDescription(const QVariantMap &option) const
{
    if (option.isEmpty())
        return {};
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    const QVariantList choices = option.value(QStringLiteral("choices")).toList();
    for (const QVariant &entry : choices) {
        const QVariantMap choice = entry.toMap();
        if (choice.value(QStringLiteral("value")).toString() == current)
            return choice.value(QStringLiteral("description")).toString();
    }
    return {};
}

QString StatusStrip::modelName() const
{
    const QVariantMap option = modelOption();
    const QString label = choiceLabel(option);
    if (!label.isEmpty())
        return label;
    if (option.isEmpty())
        return {};

    const QString raw = option.value(QStringLiteral("currentValue")).toString();
    if (raw.isEmpty())
        return {};
    // ACP's values are opaque strings; DSH encodes a route as the [provider,
    // model] JSON pair. Name the model id for the pair, the value for anything
    // else (main.qml:3350).
    if (raw.startsWith(QLatin1Char('[')) || raw.startsWith(QLatin1Char('{'))) {
        const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
        const QJsonArray route = document.array();
        if (route.size() == 2)
            return route.at(1).toVariant().toString();
    }
    return raw;
}

QString StatusStrip::selectedProvider() const
{
    const QVariantMap option = modelOption();
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    if (current.isEmpty())
        return {};
    if (current.startsWith(QLatin1Char('[')) || current.startsWith(QLatin1Char('{'))) {
        const QJsonArray route = QJsonDocument::fromJson(current.toUtf8()).array();
        if (route.size() == 2)
            return route.at(0).toVariant().toString();
        return {};
    }
    for (const QVariant &entry : option.value(QStringLiteral("choices")).toList()) {
        const QVariantMap choice = entry.toMap();
        if (choice.value(QStringLiteral("value")).toString() == current)
            return choice.value(QStringLiteral("provider")).toString();
    }
    return {};
}

QString StatusStrip::selectedModelId() const
{
    const QVariantMap option = modelOption();
    const QString current = option.value(QStringLiteral("currentValue")).toString();
    if (current.isEmpty())
        return {};
    if (current.startsWith(QLatin1Char('[')) || current.startsWith(QLatin1Char('{'))) {
        const QJsonArray route = QJsonDocument::fromJson(current.toUtf8()).array();
        if (route.size() == 2)
            return route.at(1).toVariant().toString();
    }
    return current;
}

QString StatusStrip::selectedModelName() const
{
    const QString id = selectedModelId();
    const QVariantMap option = modelOption();
    for (const QVariant &entry : option.value(QStringLiteral("choices")).toList()) {
        const QVariantMap choice = entry.toMap();
        if (choice.value(QStringLiteral("value")).toString() != id)
            continue;
        const QString model = choice.value(QStringLiteral("model")).toString();
        if (!model.isEmpty())
            return model;
        const QString label = choice.value(QStringLiteral("label")).toString();
        if (!label.isEmpty())
            return label;
        break;
    }
    return id;
}

QVariantMap StatusStrip::activeModelLoad() const
{
    const QVariantList loads = m_facade->modelLoads();
    if (loads.isEmpty())
        return {};
    const QString provider = selectedProvider();
    if (!provider.isEmpty() && !localProvider(provider))
        return {};
    const QString id = selectedModelId();
    const QString name = selectedModelName();
    const QString label = modelName();
    if (id.isEmpty() && name.isEmpty())
        return {};
    for (const QVariant &entry : loads) {
        const QVariantMap load = entry.toMap();
        const QString tenant = load.value(QStringLiteral("tenant")).toString();
        const QString model = load.value(QStringLiteral("model")).toString();
        if (modelNamesMatch(model, name) || modelNamesMatch(model, id)
                || modelNamesMatch(model, label))
            return load;
        if (!tenant.isEmpty()
            && (tenant.compare(provider, Qt::CaseInsensitive) == 0
                || modelNamesMatch(tenant, provider) || modelNamesMatch(tenant, name)))
            return load;
    }
    QVariantMap onlyActive;
    int activeCount = 0;
    QVariantMap onlyFailed;
    int failedCount = 0;
    for (const QVariant &entry : loads) {
        const QVariantMap load = entry.toMap();
        const QString phase = modelPhase(load);
        if (phase == QLatin1String("failed")) {
            ++failedCount;
            onlyFailed = load;
        } else if (phase == QLatin1String("loading")
                   || phase == QLatin1String("prefilling")
                   || phase == QLatin1String("generating")) {
            ++activeCount;
            onlyActive = load;
        }
    }
    if (activeCount == 1)
        return onlyActive;
    if (activeCount == 0 && failedCount == 1)
        return onlyFailed;
    if (loads.size() == 1)
        return loads.first().toMap();
    return {};
}

QString StatusStrip::modelPhase(const QVariantMap &load) const
{
    if (load.isEmpty())
        return {};
    const QString state = load.value(QStringLiteral("state")).toString();
    const QString phase = load.value(QStringLiteral("phase")).toString();
    if (!load.value(QStringLiteral("error")).toString().isEmpty()
            || state == QLatin1String("failed")
            || phase == QLatin1String("failed"))
        return QStringLiteral("failed");
    if (state == QLatin1String("loading") || phase == QLatin1String("loading")
            || phase == QLatin1String("queued"))
        return QStringLiteral("loading");
    if (phase == QLatin1String("prefilling") || phase == QLatin1String("generating"))
        return phase;
    if (state == QLatin1String("running")) {
        if (!load.value(QStringLiteral("active")).toBool())
            return QStringLiteral("idle");
        if (load.value(QStringLiteral("isProcessing")).toBool()) {
            const qint64 tokens = load.value(QStringLiteral("promptTokens")).toLongLong();
            const qint64 done = load.value(QStringLiteral("promptProcessed")).toLongLong();
            if (tokens > 0 && done >= tokens)
                return QStringLiteral("generating");
            return QStringLiteral("prefilling");
        }
        return QStringLiteral("prefilling");
    }
    if (state == QLatin1String("idle") || phase == QLatin1String("ready"))
        return QStringLiteral("idle");
    if (state == QLatin1String("stopped"))
        return QStringLiteral("stopped");
    return {};
}

QString StatusStrip::modelLoadCard(const QVariantMap &load) const
{
    QStringList lines;
    const QString model = load.value(QStringLiteral("model")).toString();
    const QString tenant = load.value(QStringLiteral("tenant")).toString();
    QString head = model.isEmpty() ? QStringLiteral("local model") : model;
    if (!tenant.isEmpty())
        head += QStringLiteral(" on %1").arg(tenant);
    lines.append(head);

    const QString state = load.value(QStringLiteral("state")).toString();
    const QString phase = load.value(QStringLiteral("phase")).toString();
    const QString word = modelPhase(load);
    if (!word.isEmpty()) {
        QString line = QStringLiteral("state: ") + word;
        if (!state.isEmpty() && state != word)
            line += QStringLiteral(" (") + state + QStringLiteral(")");
        if (!phase.isEmpty() && phase != state && phase != word)
            line += QStringLiteral(" · load ") + phase;
        lines.append(line);
    }

    const int percent = modelLoadPercent(load);
    if (percent >= 0) {
        const QString source =
            modelLoadProgressSource(load.value(QStringLiteral("progressSource")).toString());
        QString line = QStringLiteral("progress: %1%").arg(percent);
        if (!source.isEmpty())
            line += QStringLiteral(" · ") + source;
        lines.append(line);
    }
    if (load.contains(QStringLiteral("prefillPct"))) {
        const int prefill = qBound(0, qRound(load.value(QStringLiteral("prefillPct")).toDouble()), 100);
        if (prefill > 0 && prefill < 100)
            lines.append(QStringLiteral("prefill: %1%").arg(prefill));
    }
    if (load.contains(QStringLiteral("kvPct"))) {
        const int kv = qBound(0, qRound(load.value(QStringLiteral("kvPct")).toDouble()), 100);
        lines.append(QStringLiteral("context: %1% full").arg(kv));
    }
    const int inFlight = load.value(QStringLiteral("inFlight")).toInt();
    if (inFlight > 0)
        lines.append(QStringLiteral("in flight: %1").arg(inFlight));
    const QString device = load.value(QStringLiteral("device")).toString();
    const QString group = load.value(QStringLiteral("group")).toString();
    if (!device.isEmpty() || !group.isEmpty()) {
        QString detail = device;
        if (!group.isEmpty())
            detail += detail.isEmpty() ? group : QStringLiteral(" · ") + group;
        lines.append(QStringLiteral("device: ") + detail);
    }
    const QString kind = load.value(QStringLiteral("kind")).toString();
    if (!kind.isEmpty())
        lines.append(QStringLiteral("kind: ") + kind);
    const QString policy = load.value(QStringLiteral("policy")).toString();
    if (!policy.isEmpty())
        lines.append(QStringLiteral("policy: ") + policy);
    const int vram = load.value(QStringLiteral("vramMb")).toInt();
    if (vram > 0)
        lines.append(QStringLiteral("vram: %1 MiB").arg(vram));
    const qint64 nctx = load.value(QStringLiteral("nCtx")).toLongLong();
    if (nctx > 0)
        lines.append(QStringLiteral("context: %1 tokens").arg(nctx));
    const QString quant = load.value(QStringLiteral("quant")).toString();
    if (!quant.isEmpty())
        lines.append(QStringLiteral("quant: ") + quant);
    const double reload = load.value(QStringLiteral("reloadS")).toDouble();
    if (reload > 0)
        lines.append(QStringLiteral("reload: %1 s").arg(reload, 0, 'f', 1));
    const QString line = load.value(QStringLiteral("line")).toString();
    if (!line.isEmpty())
        lines.append(line);
    const QString error = load.value(QStringLiteral("error")).toString();
    if (!error.isEmpty())
        lines.append(QStringLiteral("error: ") + error);
    return lines.join(QStringLiteral("\n"));
}

void StatusStrip::refreshModelLoad(const QVariantMap &load)
{
    const QString phase = modelPhase(load);
    const bool show = !phase.isEmpty();
    m_load->setVisible(show);
    if (!show)
        return;

    Theme *theme = Theme::instance();
    const bool failed = phase == QLatin1String("failed");
    const int maxWidth = 20 * theme->space6();
    QString model = load.value(QStringLiteral("model")).toString();
    if (model.isEmpty())
        model = load.value(QStringLiteral("tenant")).toString();
    if (model.isEmpty())
        model = QStringLiteral("local model");

    QString percent;
    if (phase == QLatin1String("loading")) {
        const int value = modelLoadPercent(load);
        if (value >= 0)
            percent = QStringLiteral(" \u00b7 %1%").arg(value);
    } else if (phase == QLatin1String("prefilling")
               && load.contains(QStringLiteral("prefillPct"))) {
        const int value = qBound(0, qRound(load.value(QStringLiteral("prefillPct")).toDouble()), 100);
        if (value > 0 && value < 100)
            percent = QStringLiteral(" \u00b7 %1%").arg(value);
    }

    QString text;
    if (failed) {
        text = QStringLiteral("load failed");
    } else if (phase == QLatin1String("loading")) {
        const QString prefix = QStringLiteral("loading ");
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        const int content = qMax(0, maxWidth - 2 * theme->space3());
        const int tail = metrics.horizontalAdvance(prefix) + metrics.horizontalAdvance(percent);
        const QString name = metrics.elidedText(model, Qt::ElideMiddle, qMax(0, content - tail));
        text = prefix + name + percent;
    } else if (phase == QLatin1String("prefilling")) {
        text = QStringLiteral("prefilling") + percent;
    } else if (phase == QLatin1String("generating")) {
        text = QStringLiteral("generating");
    } else {
        text = phase;
    }

    QColor colour = failed ? theme->danger() : theme->fg();
    if (!failed && phase != QLatin1String("loading")
        && phase != QLatin1String("prefilling") && phase != QLatin1String("generating"))
        colour = theme->dim();
    m_load->setValue(QString(), text, colour, maxWidth);
    m_load->setToolTip(modelLoadCard(load));
}

void StatusStrip::openEstateMenu()
{
    QMenu *menu = Chrome::shellMenu(this);
    Chrome::deleteOnHide(menu);
    Chrome::addCaption(menu, QStringLiteral("Local engine"));

    const QVariantList loads = m_facade->modelLoads();
    if (loads.isEmpty()) {
        Chrome::addEmptyRow(menu, QStringLiteral("No local engine status"));
    } else {
        for (const QVariant &entry : loads) {
            const QVariantMap load = entry.toMap();
            const QString tenant = load.value(QStringLiteral("tenant")).toString();
            const QString model = load.value(QStringLiteral("model")).toString();
            const QString phase = modelPhase(load);
            QString label = model.isEmpty() ? tenant : model;
            if (label.isEmpty())
                label = QStringLiteral("local model");
            if (!tenant.isEmpty() && !model.isEmpty())
                label = tenant + QStringLiteral(" · ") + model;
            if (!phase.isEmpty())
                label += QStringLiteral(": ") + phase;
            QAction *action = menu->addAction(Chrome::menuText(label));
            action->setEnabled(false);
            action->setToolTip(modelLoadCard(load));
        }
    }
    menu->addSeparator();
    QAction *refresh = menu->addAction(Chrome::menuText(QStringLiteral("Refresh")));
    connect(refresh, &QAction::triggered, this, [this]() { m_facade->refreshEstate(); });

    Chrome::popupMenuAt(QRect(m_load->mapToGlobal(QPoint(0, 0)), m_load->size()), menu, true);
}

QString StatusStrip::effortLabel() const
{
    const QVariantMap option = effortOption();
    if (option.isEmpty() || option.value(QStringLiteral("choices")).toList().isEmpty())
        return {};
    const QString label = choiceLabel(option);
    // A value the catalog cannot name falls back to the value itself rather than
    // leaving an empty bordered box on the strip.
    return label.isEmpty() ? option.value(QStringLiteral("currentValue")).toString() : label;
}

QString StatusStrip::accessLevelName() const
{
    const QString level = m_facade->permissionPreset();
    if (level.isEmpty())
        return {};

    const QVariantList presets = m_facade->permissionPresets();
    for (const QVariant &entry : presets) {
        const QVariantMap preset = entry.toMap();
        if (preset.value(QStringLiteral("value")).toString() == level) {
            const QString name = preset.value(QStringLiteral("name")).toString();
            return preset.value(QStringLiteral("dangerous")).toBool()
                       ? QStringLiteral("\u26a0 ") + name
                       : name;
        }
    }
    if (level == QLatin1String("full-access"))
        return QStringLiteral("\u26a0 Full access");
    // The key is still the truth: "read-only", "auto" and "custom" all read as
    // themselves, with the hyphen opened to a space so a key reads as a phrase.
    QString derived = level;
    derived.replace(QLatin1Char('-'), QLatin1Char(' '));
    return derived;
}

QString StatusStrip::accessDescription() const
{
    const QString level = m_facade->permissionPreset();
    if (level.isEmpty())
        return {};
    const QVariantList presets = m_facade->permissionPresets();
    for (const QVariant &entry : presets) {
        const QVariantMap preset = entry.toMap();
        if (preset.value(QStringLiteral("value")).toString() == level) {
            if (preset.value(QStringLiteral("dangerous")).toBool())
                return preset.value(QStringLiteral("warning")).toString();
            return preset.value(QStringLiteral("description")).toString();
        }
    }
    return {};
}

bool StatusStrip::accessDangerous() const
{
    const QString level = m_facade->permissionPreset();
    if (level == QLatin1String("full-access"))
        return true;
    for (const QVariant &entry : m_facade->permissionPresets()) {
        const QVariantMap preset = entry.toMap();
        if (preset.value(QStringLiteral("value")).toString() == level)
            return preset.value(QStringLiteral("dangerous")).toBool();
    }
    return false;
}

void StatusStrip::onDangerousPresetRequested(const QString &id, const QString &warning)
{
    if (!m_facade)
        return;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Full access"));
    box.setText(warning);
    box.setInformativeText(QStringLiteral("Select this preset?"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes)
        return;
    QObject *transport = m_facade->transport();
    if (transport == nullptr)
        return;
    QMetaObject::invokeMethod(transport, "confirmPermissionPreset",
                              Q_ARG(QString, id));
}

QString StatusStrip::jobText() const
{
    const int total = m_facade->jobs().size();
    const int live = m_facade->liveJobCount();
    // A live job is the state the owner asked to see, so the chip counts those
    // while any is unsettled; the roster's total is the answer once nothing is
    // running (main.qml:4176).
    if (live > 0) {
        return live == 1 ? QStringLiteral("1 job running")
                         : QStringLiteral("%1 jobs running").arg(live);
    }
    return total == 1 ? QStringLiteral("1 job") : QStringLiteral("%1 jobs").arg(total);
}

bool StatusStrip::jobsLive() const
{
    return m_facade->liveJobCount() > 0;
}

int StatusStrip::contextPercent() const
{
    // The web's own rounded percentage: min(100, round(used / window * 100)). A
    // window of zero is the case the cell is hidden for.
    const int size = m_facade->contextSize();
    if (size <= 0)
        return 0;
    return qMin(100, qRound(100.0 * m_facade->contextUsed() / size));
}

void StatusStrip::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_facade->setEstatePolling(true);
}

void StatusStrip::hideEvent(QHideEvent *event)
{
    m_facade->setEstatePolling(false);
    QWidget::hideEvent(event);
}

void StatusStrip::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);

    painter.fillRect(rect(), theme->raised());

    // Two rules, one per surface: the accent on top is what says "new strip" at
    // near-black, and the border underneath is the seam against the transcript
    // above it. Both are 1 px fills, never text.
    painter.fillRect(QRect(0, 0, width(), theme->hairline()), theme->accent());
    painter.fillRect(QRect(0, height() - theme->hairline(), width(), theme->hairline()),
                     theme->border());
}
