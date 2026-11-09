#include "WorkspacePickerDialog.h"

#include "ClientFacade.h"
#include "ShellChrome.h"
#include "Theme.h"

#include <QCheckBox>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

namespace {

QIcon directoryIcon(const QString &iconDirectory)
{
    if (!iconDirectory.isEmpty()) {
        const QString file = iconDirectory + QStringLiteral("/folder.svg");
        if (QFile::exists(file))
            return QIcon(file);
    }

    Theme *theme = Theme::instance();
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(theme->dim(), 1.3));
    painter.setBrush(Qt::NoBrush);
    QPainterPath folder;
    folder.moveTo(2.0, 12.6);
    folder.lineTo(2.0, 4.2);
    folder.lineTo(6.0, 4.2);
    folder.lineTo(7.6, 6.0);
    folder.lineTo(14.0, 6.0);
    folder.lineTo(14.0, 12.6);
    folder.closeSubpath();
    painter.drawPath(folder);
    return QIcon(pixmap);
}

QPushButton *pushButton(QWidget *parent, const QString &text)
{
    auto *control = new QPushButton(text, parent);
    control->setFont(Chrome::uiFont(Theme::instance()->fontUI()));
    control->setAutoDefault(false);
    control->setDefault(false);
    return control;
}

QLabel *lineLabel(QWidget *parent, const QColor &colour, const QFont &font)
{
    auto *label = new QLabel(parent);
    label->setFont(font);
    label->setFixedHeight(Theme::instance()->rowHeight());
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
    return label;
}

}

WorkspacePickerDialog::WorkspacePickerDialog(ClientFacade *facade, const QString &iconDirectory,
                                             QWidget *parent)
    : QDialog(parent)
    , m_facade(facade)
    , m_iconDirectory(iconDirectory)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("workspacePickerDialog"));
    setWindowTitle(QStringLiteral("Add workspace"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(560, 480);
    setMinimumSize(420, 340);
    setSizeGripEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space4());

    auto *heading = new QLabel(QStringLiteral("Add workspace"), this);
    heading->setFont(Chrome::uiFont(theme->fontProse() + 2, true));
    QPalette headingPalette = heading->palette();
    headingPalette.setColor(QPalette::WindowText, theme->fg());
    heading->setPalette(headingPalette);
    layout->addWidget(heading);

    auto *subheading = new QLabel(QStringLiteral("Choose the folder to add."), this);
    subheading->setFont(Chrome::uiFont(theme->fontData()));
    QPalette subheadingPalette = subheading->palette();
    subheadingPalette.setColor(QPalette::WindowText, theme->dim());
    subheading->setPalette(subheadingPalette);
    layout->addWidget(subheading);

    m_crumbScroll = new QScrollArea(this);
    m_crumbScroll->setObjectName(QStringLiteral("workspacePickerCrumbs"));
    m_crumbScroll->setFrameShape(QFrame::NoFrame);
    m_crumbScroll->setWidgetResizable(true);
    m_crumbScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_crumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_crumbScroll->setFixedHeight(theme->controlHeight() + theme->space2());
    m_crumbScroll->setPalette(Theme::palette());
    m_crumbHost = new QWidget(m_crumbScroll);
    m_crumbHost->setPalette(Theme::palette());
    m_crumbLayout = new QHBoxLayout(m_crumbHost);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(theme->space2());
    m_crumbScroll->setWidget(m_crumbHost);
    layout->addWidget(m_crumbScroll);

    m_entries = new QListWidget(this);
    m_entries->setObjectName(QStringLiteral("workspacePickerEntries"));
    m_entries->setFont(Chrome::uiFont(theme->fontUI()));
    m_entries->setIconSize(QSize(16, 16));
    m_entries->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entries->setUniformItemSizes(true);
    m_entries->setFrameShape(QFrame::NoFrame);
    m_entries->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_entries->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    Chrome::styleScrollBar(m_entries->verticalScrollBar());
    QPalette entryPalette = m_entries->palette();
    entryPalette.setColor(QPalette::Base, theme->panel());
    entryPalette.setColor(QPalette::Text, theme->fg());
    entryPalette.setColor(QPalette::Highlight, theme->accent());
    entryPalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_entries->setPalette(entryPalette);
    m_entries->viewport()->setPalette(entryPalette);
    layout->addWidget(m_entries, 1);

    m_state = lineLabel(this, theme->dim(), Chrome::uiFont(theme->fontData()));
    m_state->setObjectName(QStringLiteral("workspacePickerState"));
    layout->addWidget(m_state);

    m_truncated = lineLabel(this, theme->caption(), Chrome::uiFont(theme->fontData()));
    m_truncated->setObjectName(QStringLiteral("workspacePickerTruncated"));
    layout->addWidget(m_truncated);

    m_notice = lineLabel(this, theme->danger(), Chrome::uiFont(theme->fontData()));
    m_notice->setObjectName(QStringLiteral("workspacePickerNotice"));
    m_notice->setTextFormat(Qt::PlainText);
    layout->addWidget(m_notice);

    auto *createRow = new QHBoxLayout();
    createRow->setContentsMargins(0, 0, 0, 0);
    createRow->setSpacing(theme->space3());
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("workspacePickerName"));
    m_name->setFont(Chrome::uiFont(theme->fontUI()));
    m_name->setPlaceholderText(QStringLiteral("New folder name"));
    QPalette namePalette = m_name->palette();
    namePalette.setColor(QPalette::Base, theme->fieldFill());
    namePalette.setColor(QPalette::Text, theme->fg());
    namePalette.setColor(QPalette::Highlight, theme->accent());
    namePalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_name->setPalette(namePalette);
    createRow->addWidget(m_name, 1);
    m_create = pushButton(this, QStringLiteral("Create"));
    m_create->setObjectName(QStringLiteral("workspacePickerCreate"));
    createRow->addWidget(m_create);
    layout->addLayout(createRow);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(theme->space3());
    m_showHidden = new QCheckBox(QStringLiteral("Show hidden"), this);
    m_showHidden->setObjectName(QStringLiteral("workspacePickerShowHidden"));
    m_showHidden->setFont(Chrome::uiFont(theme->fontUI()));
    QPalette hiddenPalette = m_showHidden->palette();
    hiddenPalette.setColor(QPalette::WindowText, theme->dim());
    hiddenPalette.setColor(QPalette::ButtonText, theme->dim());
    m_showHidden->setPalette(hiddenPalette);
    actions->addWidget(m_showHidden);
    actions->addStretch(1);
    auto *cancel = pushButton(this, QStringLiteral("Cancel"));
    m_add = pushButton(this, QStringLiteral("Add this folder"));
    m_add->setObjectName(QStringLiteral("workspacePickerAdd"));
    actions->addWidget(cancel);
    actions->addWidget(m_add);
    layout->addLayout(actions);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_add, &QPushButton::clicked, this, &WorkspacePickerDialog::addCurrentDirectory);
    connect(m_create, &QPushButton::clicked, this, &WorkspacePickerDialog::submitDirectory);
    connect(m_name, &QLineEdit::returnPressed, this, &WorkspacePickerDialog::submitDirectory);
    connect(m_name, &QLineEdit::textChanged, this, &WorkspacePickerDialog::refreshState);
    connect(m_showHidden, &QCheckBox::toggled, this, [this]() { applyListing(); });
    connect(m_entries, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item == nullptr)
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            m_facade->browseDirectory(path);
    });

    connect(m_facade, &ClientFacade::directoryListingChanged, this,
            &WorkspacePickerDialog::applyListing);
    connect(m_facade, &ClientFacade::directoryPickLoadingChanged, this,
            &WorkspacePickerDialog::refreshState);
    connect(m_facade, &ClientFacade::directoryPickNoticeChanged, this,
            &WorkspacePickerDialog::refreshState);
}

void WorkspacePickerDialog::showPicker()
{
    m_name->clear();
    applyListing();
    refreshState();
    open();
    raise();
    activateWindow();
    m_entries->setFocus();
}

void WorkspacePickerDialog::applyListing()
{
    const QVariantMap listing = m_facade->directoryListing();
    m_path = listing.value(QStringLiteral("path")).toString();
    rebuildCrumbs(listing);
    rebuildEntries(listing);
    refreshState();
}

void WorkspacePickerDialog::rebuildCrumbs(const QVariantMap &listing)
{
    Theme *theme = Theme::instance();

    while (QLayoutItem *item = m_crumbLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    QVariantList crumbs = listing.value(QStringLiteral("crumbs")).toList();
    const QString currentPath = listing.value(QStringLiteral("path")).toString();
    if (crumbs.isEmpty() && !currentPath.isEmpty())
        crumbs.append(QVariantMap{{QStringLiteral("name"), currentPath},
                                  {QStringLiteral("path"), currentPath}});
    for (int index = 0; index < crumbs.size(); ++index) {
        const QVariantMap crumb = crumbs.at(index).toMap();
        const QString name = crumb.value(QStringLiteral("name")).toString();
        const QString path = crumb.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;

        if (m_crumbLayout->count() > 0) {
            auto *separator = new QLabel(QStringLiteral("\u203a"), m_crumbHost);
            separator->setFont(Chrome::uiFont(theme->fontData()));
            QPalette separatorPalette = separator->palette();
            separatorPalette.setColor(QPalette::WindowText, theme->faint());
            separator->setPalette(separatorPalette);
            m_crumbLayout->addWidget(separator);
        }

        auto *crumbButton = new QPushButton(name, m_crumbHost);
        crumbButton->setObjectName(QStringLiteral("workspacePickerCrumb"));
        crumbButton->setFlat(true);
        crumbButton->setAutoDefault(false);
        crumbButton->setDefault(false);
        crumbButton->setCursor(Qt::PointingHandCursor);
        const bool current = index == crumbs.size() - 1;
        crumbButton->setFont(Chrome::uiFont(theme->fontData(), current));
        QPalette crumbPalette = crumbButton->palette();
        crumbPalette.setColor(QPalette::ButtonText, current ? theme->fg() : theme->caption());
        crumbPalette.setColor(QPalette::WindowText, current ? theme->fg() : theme->caption());
        crumbButton->setPalette(crumbPalette);
        connect(crumbButton, &QPushButton::clicked, this,
                [this, path]() { m_facade->browseDirectory(path); });
        m_crumbLayout->addWidget(crumbButton);
    }

    m_crumbLayout->addStretch(1);

    QTimer::singleShot(0, m_crumbScroll, [this]() {
        QScrollBar *bar = m_crumbScroll->horizontalScrollBar();
        bar->setValue(bar->maximum());
    });
}

void WorkspacePickerDialog::rebuildEntries(const QVariantMap &listing)
{
    Theme *theme = Theme::instance();
    const bool showHidden = m_showHidden->isChecked();
    const QIcon icon = directoryIcon(m_iconDirectory);

    m_entries->clear();
    m_visibleCount = 0;

    for (const QVariant &value : listing.value(QStringLiteral("entries")).toList()) {
        const QVariantMap entry = value.toMap();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        const bool hidden = entry.value(QStringLiteral("hidden")).toBool();
        if (hidden && !showHidden)
            continue;

        auto *item = new QListWidgetItem(icon, entry.value(QStringLiteral("name")).toString());
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        item->setSizeHint(QSize(0, theme->rowHeight()));
        if (hidden)
            item->setForeground(theme->faint());
        m_entries->addItem(item);
        ++m_visibleCount;
    }

    m_truncated->setText(listing.value(QStringLiteral("truncated")).toBool()
                             ? QStringLiteral("More directories than shown")
                             : QString());
}

void WorkspacePickerDialog::refreshState()
{
    m_loading = m_facade->directoryPickLoading();
    const bool hasListing = !m_path.isEmpty();
    if (m_loading)
        m_state->setText(QStringLiteral("Reading\u2026"));
    else if (hasListing && m_visibleCount == 0)
        m_state->setText(QStringLiteral("No subdirectories"));
    else
        m_state->setText(QString());

    m_notice->setText(m_facade->directoryPickNotice());
    m_notice->setToolTip(m_facade->directoryPickNotice());

    m_name->setEnabled(!m_loading && hasListing);
    m_create->setEnabled(!m_loading && hasListing && !m_name->text().trimmed().isEmpty());
    m_add->setEnabled(hasListing && !m_loading);
    m_showHidden->setEnabled(!m_loading);

    m_entries->setCursor(m_loading ? Qt::BusyCursor : Qt::ArrowCursor);
}

void WorkspacePickerDialog::submitDirectory()
{
    const QString name = m_name->text().trimmed();
    if (name.isEmpty() || m_path.isEmpty())
        return;
    m_facade->createDirectory(name);
    m_name->clear();
    m_entries->setFocus();
}

void WorkspacePickerDialog::addCurrentDirectory()
{
    if (m_path.isEmpty())
        return;
    m_facade->addWorkspaceAt(m_path);
    accept();
}
