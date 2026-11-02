#include "ChangesDialog.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSpacerItem>
#include <QStringList>
#include <QVBoxLayout>

namespace {

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

QPushButton *dangerButton(QWidget *parent, const QString &text)
{
    QPushButton *control = pushButton(parent, text);
    QPalette palette = control->palette();
    palette.setColor(QPalette::ButtonText, Theme::instance()->danger());
    control->setPalette(palette);
    return control;
}

}

ChangesDialog::ChangesDialog(ClientFacade *facade, QWidget *parent)
    : QDialog(parent)
    , m_facade(facade)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("changesDialog"));
    setWindowTitle(QStringLiteral("Changed files"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(520, 380);
    setMinimumSize(440, 260);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space5());

    m_title = textLabel(this, QStringLiteral("Changed files"), theme->fg(),
                        Chrome::uiFont(theme->fontProse() + 2, true));
    layout->addWidget(m_title);
    layout->addWidget(textLabel(this,
                                QStringLiteral("Click one to open it in the Files panel. This is "
                                               "the current content, not a diff: the change watch "
                                               "reports versions, never the previous text."),
                                theme->dim(), Chrome::uiFont(theme->fontData())));
    m_empty = textLabel(this,
                        QStringLiteral("Edit a file in this workspace and it will appear here."),
                        theme->caption(), Chrome::uiFont(theme->fontData()));
    layout->addWidget(m_empty);

    m_list = new QListWidget(this);
    Chrome::styleScrollBar(m_list->verticalScrollBar());
    m_list->setFont(Chrome::monoFont(theme->fontData()));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setTextElideMode(Qt::ElideLeft);
    m_list->setMinimumHeight(160);
    QPalette listPalette = m_list->palette();
    listPalette.setColor(QPalette::Base, theme->panel());
    listPalette.setColor(QPalette::Text, theme->fg());
    listPalette.setColor(QPalette::Highlight, theme->accent());
    listPalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_list->setPalette(listPalette);
    layout->addWidget(m_list, 1);

    m_bottomSpacer = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Minimum);
    layout->addSpacerItem(m_bottomSpacer);

    m_actions = new QWidget(this);
    auto *actions = new QHBoxLayout(m_actions);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(theme->space3());
    m_clear = dangerButton(m_actions, QStringLiteral("Clear"));
    auto *close = pushButton(m_actions, QStringLiteral("Close"));
    close->setDefault(true);
    actions->addWidget(m_clear);
    actions->addStretch(1);
    actions->addWidget(close);
    layout->addWidget(m_actions);

    m_confirm = new QWidget(this);
    auto *confirm = new QHBoxLayout(m_confirm);
    confirm->setContentsMargins(0, 0, 0, 0);
    confirm->setSpacing(theme->space3());
    auto *question = textLabel(m_confirm,
                               QStringLiteral("Clear the list? The watch starts again from now."),
                               theme->fg(), Chrome::uiFont(theme->fontData()));
    m_keep = pushButton(m_confirm, QStringLiteral("Keep"));
    auto *clear = dangerButton(m_confirm, QStringLiteral("Clear"));
    confirm->addWidget(question, 1);
    confirm->addWidget(m_keep);
    confirm->addWidget(clear);
    layout->addWidget(m_confirm);
    m_confirm->hide();

    connect(m_clear, &QPushButton::clicked, this, [this]() { setConfirming(true); });
    connect(m_keep, &QPushButton::clicked, this, [this]() { setConfirming(false); });
    connect(clear, &QPushButton::clicked, this, &ChangesDialog::confirmClear);
    connect(close, &QPushButton::clicked, this, &ChangesDialog::reject);
    connect(m_list, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *) { openCurrent(); });
    connect(m_list, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *) { openCurrent(); });

    if (m_facade != nullptr)
        connect(m_facade, &ClientFacade::changedFilesChanged, this, &ChangesDialog::reload);

    reload();
}

void ChangesDialog::showDialog()
{
    reload();
    setConfirming(false);
    open();
    raise();
    activateWindow();
}

void ChangesDialog::reject()
{
    if (m_confirm->isVisible()) {
        setConfirming(false);
        return;
    }
    QDialog::reject();
}

void ChangesDialog::reload()
{
    const QStringList files = m_facade != nullptr ? m_facade->changedFiles() : QStringList();

    m_title->setText(files.size() == 1
                         ? QStringLiteral("1 file changed")
                         : files.isEmpty() ? QStringLiteral("No files changed")
                                           : QStringLiteral("%1 files changed").arg(files.size()));

    m_list->clear();
    for (const QString &path : files) {
        auto *item = new QListWidgetItem(path, m_list);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    m_empty->setVisible(files.isEmpty());
    m_list->setVisible(!files.isEmpty());
    m_clear->setEnabled(!files.isEmpty());
    m_bottomSpacer->changeSize(0, 0, QSizePolicy::Minimum,
                              files.isEmpty() ? QSizePolicy::Expanding
                                              : QSizePolicy::Minimum);
    layout()->invalidate();
    if (files.isEmpty())
        setConfirming(false);
}

void ChangesDialog::setConfirming(bool confirming)
{
    if (confirming && !m_clear->isEnabled())
        confirming = false;
    m_confirm->setVisible(confirming);
    m_actions->setVisible(!confirming);
    if (confirming)
        m_keep->setFocus();
}

void ChangesDialog::confirmClear()
{
    if (m_facade == nullptr)
        return;
    m_facade->clearChangedFiles();
    qInfo() << "changes dialog: clearChangedFiles() confirmed";
    setConfirming(false);
}

void ChangesDialog::openCurrent()
{
    QListWidgetItem *item = m_list->currentItem();
    if (item == nullptr)
        return;
    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty())
        return;
    qInfo().nospace() << "changes dialog: open " << path;
    emit fileRequested(path);
    accept();
}
