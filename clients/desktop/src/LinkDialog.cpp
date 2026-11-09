#include "LinkDialog.h"

#include "ShellChrome.h"
#include "Theme.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int CopiedConfirmationMs = 1600;

QLabel *textLabel(QWidget *parent, const QString &text, const QColor &colour,
                  const QFont &font)
{
    auto *label = new QLabel(text, parent);
    label->setFont(font);
    label->setWordWrap(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
    return label;
}

QPushButton *pushButton(QWidget *parent, const QString &text)
{
    auto *control = new QPushButton(text, parent);
    control->setFont(Chrome::uiFont(Theme::instance()->fontUI()));
    control->setAutoDefault(false);
    control->setDefault(false);
    return control;
}

void stylePrimary(QPushButton *control, const Theme *theme)
{
    QPalette palette = control->palette();
    palette.setColor(QPalette::Button, theme->accent());
    palette.setColor(QPalette::ButtonText, theme->accentFg());
    control->setPalette(palette);
}

}

LinkDialog::LinkDialog(QWidget *parent)
    : QDialog(parent)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("linkDialog"));
    setWindowTitle(QStringLiteral("Open link"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(520, 260);
    setMinimumSize(380, 200);
    setSizeGripEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space5());

    layout->addWidget(textLabel(this, QStringLiteral("Open link"), theme->fg(),
                                Chrome::uiFont(theme->fontProse() + 2, true)));
    layout->addWidget(textLabel(this, QStringLiteral("Choose what to do with this link."),
                                theme->dim(), Chrome::uiFont(theme->fontData())));

    m_url = textLabel(this, QString(), theme->fg(), Chrome::monoFont(theme->fontData()));
    m_url->setTextFormat(Qt::PlainText);
    m_url->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_url->setMargin(theme->space3());
    layout->addWidget(m_url);

    m_note = textLabel(this, QString(), theme->caption(), Chrome::uiFont(theme->fontData()));
    layout->addWidget(m_note);
    m_note->hide();

    layout->addStretch(1);

    m_copy = pushButton(this, QStringLiteral("Copy"));
    m_inWindow = pushButton(this, QStringLiteral("Open in this window"));
    auto *cancel = pushButton(this, QStringLiteral("Cancel"));
    m_browser = pushButton(this, QStringLiteral("Open in browser"));
    stylePrimary(m_browser, theme);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(theme->space3());
    actions->addWidget(m_copy);
    actions->addWidget(m_inWindow);
    actions->addStretch(1);
    actions->addWidget(cancel);
    actions->addWidget(m_browser);
    layout->addLayout(actions);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_copy, &QPushButton::clicked, this, &LinkDialog::copy);
    connect(m_browser, &QPushButton::clicked, this, [this]() {
        emit browserRequested(QUrl(m_href));
        accept();
    });
    connect(m_inWindow, &QPushButton::clicked, this, [this]() {
        emit inWindowRequested(QUrl(m_href));
        accept();
    });
}

void LinkDialog::showLink(const QString &href)
{
    m_href = href;
    m_url->setText(href);

    const QString scheme = QUrl(href).scheme().toLower();
    const bool web = scheme == QLatin1String("http") || scheme == QLatin1String("https");
    const bool mail = scheme == QLatin1String("mailto");
    const bool local = scheme == QLatin1String("file");
    const bool openable = web || mail || local;

    m_browser->setEnabled(openable);
    m_browser->setDefault(openable);
    if (mail) {
        m_browser->setText(QStringLiteral("Open in mail app"));
        m_browser->setToolTip(QStringLiteral("Your mail application will handle this link."));
    } else if (local) {
        m_browser->setText(QStringLiteral("Open local file"));
        m_browser->setToolTip(QStringLiteral("This link points at a file on this machine."));
    } else {
        m_browser->setText(QStringLiteral("Open in browser"));
        m_browser->setToolTip(openable ? QString()
                                       : QStringLiteral("This kind of link cannot be opened."));
    }

    m_inWindow->setEnabled(web);
    m_inWindow->setToolTip(web ? QString()
                               : QStringLiteral("Only http and https links can open here."));

    if (!openable) {
        m_note->setText(QStringLiteral("This kind of link cannot be opened here; "
                                       "you can still copy it."));
        m_note->show();
    } else if (local) {
        m_note->setText(QStringLiteral("This link points at a local file, not a web page."));
        m_note->show();
    } else {
        m_note->hide();
    }

    m_copy->setText(QStringLiteral("Copy"));
    m_copy->setPalette(Theme::palette());
    m_copy->setDefault(false);

    open();
    raise();
    activateWindow();
}

void LinkDialog::copy()
{
    if (m_href.isEmpty())
        return;

    QGuiApplication::clipboard()->setText(m_href);
    QPalette palette = m_copy->palette();
    palette.setColor(QPalette::ButtonText, Theme::instance()->ok());
    m_copy->setPalette(palette);
    m_copy->setText(QStringLiteral("Copied"));
    QTimer::singleShot(CopiedConfirmationMs, this, [this]() {
        m_copy->setText(QStringLiteral("Copy"));
        m_copy->setPalette(Theme::palette());
    });
}
