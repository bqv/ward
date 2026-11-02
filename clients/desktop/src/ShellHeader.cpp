#include "ShellHeader.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QToolButton>

// ---- the brand and the connection dot --------------------------------------
//
// One widget for the dot and the word: they are one mark on the bar, and drawing
// them together is what keeps the dot's centre on the text's optical centre rather
// than on a layout's idea of it. The dot is the transport's state and nothing
// else, the same four states and the same four tokens as the QML header
// (main.qml:744): idle `dim`, starting `warn`, ready `ok`, failed `danger`.
class ShellHeader::BrandMark : public QWidget
{
public:
    explicit BrandMark(ClientFacade *facade, QWidget *parent)
        : QWidget(parent)
        , m_facade(facade)
    {
    }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        const QFontMetrics metrics(Chrome::uiFont(16, true));
        const int dot = 2 * theme->space2();   // 8 px, as in main.qml
        return QSize(dot + theme->space4() + metrics.horizontalAdvance(QStringLiteral("ward")),
                     metrics.height());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int dot = 2 * theme->space2();
        const QRect dotRect(0, (height() - dot) / 2, dot, dot);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dotColour());
        painter.drawEllipse(dotRect);

        const QRect textRect(dot + theme->space4(), 0,
                             width() - dot - theme->space4(), height());
        Chrome::paintLine(painter, textRect, Chrome::uiFont(16, true), theme->fg(),
                          QStringLiteral("ward"));
    }

private:
    QColor dotColour() const
    {
        Theme *theme = Theme::instance();
        switch (m_facade->state()) {
        case ClientFacade::Idle:       return theme->dim();
        case ClientFacade::Connecting: return theme->warn();
        case ClientFacade::Ready:      return theme->ok();
        case ClientFacade::Failed:     return theme->danger();
        }
        return theme->dim();
    }

    ClientFacade *m_facade;
};

// ---- one pane toggle -------------------------------------------------------
//
// A chip with a checkbox's behaviour. `QToolButton` is the base rather than
// `QAbstractButton` because it is the plan's control for these toggles (plan §5,
// stage 1) and it brings the checkable/tooltip/click contract; the appearance is
// entirely this class's, because a Fusion tool button is a bevel and this is a
// 20 px bordered chip.
class ShellHeader::Toggle : public QToolButton
{
public:
    Toggle(const QString &label, const QString &page, const QString &awayTooltip,
           const QString &openTooltip, QWidget *parent)
        : QToolButton(parent)
        , m_page(page)
        , m_awayTooltip(awayTooltip)
        , m_openTooltip(openTooltip)
    {
        // The label is the same word the QML header prints, and it is the
        // widget's text as well as its paint, so a screen reader and a future
        // menu both get it without a second spelling.
        setText(label);
        setObjectName(QStringLiteral("toggle-") + page);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        // The QML toggles are `MouseArea`s and take no keyboard focus; matching
        // that now keeps the tab order honest until a later slice gives the
        // header a real one.
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        refreshTooltip();
        QObject::connect(this, &QToolButton::toggled, this,
                         [this]() { refreshTooltip(); });
    }

    /// The page this toggle shows. Kept apart from `text()` so the label and the
    /// page identity cannot be re-worded apart from one another by accident.
    QString page() const { return m_page; }

    /// Sync the checked state without re-entering the shell: a programmatic check
    /// still emits `toggled`, which is where the tooltip's direction is refreshed.
    void applyChecked(bool checked) { setChecked(checked); }

    QSize sizeHint() const override
    {
        Theme *theme = Theme::instance();
        // Two pixels of slack, and it is not superstition: the label rect is the
        // chip less `space3` on each side, so a width of exactly the advance
        // leaves the painter zero room and "headless" comes out "headl...". The
        // QML chip has the same padding plus its own implicit width, which is
        // where the slack comes from there.
        const QFontMetrics metrics(Chrome::uiFont(theme->fontData()));
        return QSize(metrics.horizontalAdvance(text()) + 2 * theme->space3() + 2,
                     theme->fontData() + 2 * theme->space2());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        Theme *theme = Theme::instance();
        QPainter painter(this);

        // The lit state is exactly main.qml's: fill on hover, accent border while
        // the pane is up, label lifted to the text tier under the pointer or while
        // open and quiet `dim` at rest.
        const bool lit = isChecked();
        Chrome::paintChip(painter, rect(), underMouse(), lit);

        const QRect label = rect().adjusted(theme->space3(), 0, -theme->space3(), 0);
        Chrome::paintLine(painter, label, Chrome::uiFont(theme->fontData()),
                          underMouse() || lit ? theme->fg() : theme->dim(), text(),
                          Qt::AlignCenter);
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
    void refreshTooltip()
    {
        // The whole sentence the QML builds with a ternary on the pane's state:
        // put away and return, one tooltip per direction.
        setToolTip(isChecked() ? m_awayTooltip : m_openTooltip);
    }

    QString m_page;
    QString m_awayTooltip;
    QString m_openTooltip;
};

// ---- the bar ---------------------------------------------------------------

ShellHeader::ShellHeader(ClientFacade *facade, QWidget *parent)
    : QWidget(parent)
    , m_facade(facade)
{
    setObjectName(QStringLiteral("shellHeader"));
    setFixedHeight(Theme::instance()->headerHeight());

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(Theme::instance()->space5(), 0, Theme::instance()->space5(), 0);
    row->setSpacing(Theme::instance()->space4());

    m_brand = new BrandMark(facade, this);
    row->addWidget(m_brand);

    // The brand is held at the leading edge by a filling item. Without it a row
    // spreads its children across the width, which is the drift main.qml:784
    // records when the header lost its trailing spacer once before.
    row->addStretch(1);

    // The tooltips are main.qml's, word for word (main.qml:851, 894, 936): the
    // toggle is a way out AND back in, so each direction has its own sentence.
    m_web = new Toggle(QStringLiteral("web"), QStringLiteral("web"),
                       QStringLiteral("Put the DSH web UI away, keeping the page as it is"),
                       QStringLiteral("Show the DSH web UI in this window"), this);
    m_acp = new Toggle(QStringLiteral("acp"), QStringLiteral("acp"),
                       QStringLiteral("Put the ACP pane away, keeping its sessions running"),
                       QStringLiteral("Open the ACP sessions in this window"), this);
    m_headless = new Toggle(QStringLiteral("headless"), QStringLiteral("headless"),
                            QStringLiteral("Put the headless pane away, keeping the conversation"),
                            QStringLiteral("Run a one-shot headless task in this window"), this);

    for (Toggle *toggle : {m_web, m_acp, m_headless}) {
        row->addWidget(toggle);
        QObject::connect(toggle, &QToolButton::clicked, this,
                         [this, toggle]() { emit paneToggleRequested(toggle->page()); });
    }

    // A pane of the same protocol beside itself would be a second child for no
    // second view, which is why main.qml hides the ACP toggle when the window's
    // own transport is ACP (main.qml:873, `visible: dshTransport.transportName
    // !== "acp"`). Hidden, not disabled: it is not an unavailable control, it is
    // not a control at all on that launch.
    m_acp->setVisible(facade->transportName() != QLatin1String("acp"));

    // The dot follows the transport's state; the header owns nothing about it.
    // The word is logged too, because the dot is eight pixels of colour and this
    // is the one line that says what the header is showing in a run with no
    // screen to look at.
    QObject::connect(facade, &ClientFacade::stateChanged, this, [this]() {
        m_brand->update();
        qInfo().nospace() << "header dot: " << m_facade->stateText()
                          << " (" << m_facade->transportName() << ")";
    });
    qInfo().nospace() << "header: dot " << facade->stateText() << " ("
                      << facade->transportName() << "); toggles web, acp"
                      << (m_acp->isHidden() ? " hidden (this transport is ACP)" : "")
                      << ", headless";
}

void ShellHeader::setCurrentPage(const QString &page)
{
    // One exclusive choice: the toggles describe which page is up, they never
    // decide it. `setChecked` on its own would be enough for the paint, but a
    // programmatic check is still a state change and the tooltip's direction is
    // derived from it — `applyChecked` keeps this a pure sync through the object
    // that owns both.
    for (Toggle *toggle : {m_web, m_acp, m_headless})
        toggle->applyChecked(toggle->page() == page);

    if (page != m_page) {
        m_page = page;
        // The management page is a stack page with no toggle of its own, so
        // "none" is the whole truth for it: naming it here would read as a fourth
        // toggle that had been checked.
        const bool aPane = page == QLatin1String("web") || page == QLatin1String("acp")
                           || page == QLatin1String("headless");
        qInfo().nospace() << "header toggles: "
                          << (aPane ? page : QStringLiteral("none")) << " checked";
    }
}

void ShellHeader::paintEvent(QPaintEvent *)
{
    Theme *theme = Theme::instance();
    QPainter painter(this);

    painter.fillRect(rect(), theme->panel());

    // The seam against the content, a 1 px fill rather than a border: the same
    // hairline main.qml draws at the bottom of the header.
    painter.fillRect(QRect(0, height() - theme->hairline(), width(), theme->hairline()),
                     theme->border());
}
