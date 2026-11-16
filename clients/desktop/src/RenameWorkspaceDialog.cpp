#include "RenameWorkspaceDialog.h"

#include "ShellChrome.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
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

}

RenameWorkspaceDialog::RenameWorkspaceDialog(QWidget *parent)
    : QDialog(parent)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("renameWorkspaceDialog"));
    setWindowTitle(QStringLiteral("Rename workspace"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(420, 180);
    setMinimumSize(360, 160);
    setSizeGripEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space5());

    layout->addWidget(textLabel(this, QStringLiteral("Rename workspace"), theme->fg(),
                                Chrome::uiFont(theme->fontProse() + 2, true)));
    layout->addWidget(textLabel(this, QStringLiteral("Workspace name"), theme->caption(),
                                Chrome::monoFont(theme->fontData())));

    m_title = new QLineEdit(this);
    m_title->setObjectName(QStringLiteral("workspaceTitle"));
    m_title->setFont(Chrome::uiFont(theme->fontUI()));
    m_title->setFrame(false);
    m_title->setMinimumHeight(theme->controlHeight());
    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::Base, theme->fieldFill());
    titlePalette.setColor(QPalette::Text, theme->fg());
    titlePalette.setColor(QPalette::Highlight, theme->accent());
    titlePalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_title->setPalette(titlePalette);
    layout->addWidget(m_title);
    layout->addStretch(1);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(theme->space3());
    actions->addStretch(1);
    auto *cancel = pushButton(this, QStringLiteral("Cancel"));
    m_save = pushButton(this, QStringLiteral("Save"));
    m_save->setDefault(true);
    actions->addWidget(cancel);
    actions->addWidget(m_save);
    layout->addLayout(actions);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_save, &QPushButton::clicked, this, &RenameWorkspaceDialog::save);
    connect(m_title, &QLineEdit::textChanged, this, [this]() {
        m_save->setEnabled(!m_title->text().trimmed().isEmpty());
    });
}

void RenameWorkspaceDialog::showDialog(const QString &title)
{
    m_title->setText(title);
    m_title->selectAll();
    m_save->setEnabled(!m_title->text().trimmed().isEmpty());
    m_title->setFocus();
    open();
    raise();
    activateWindow();
}

void RenameWorkspaceDialog::save()
{
    const QString title = m_title->text().trimmed();
    if (title.isEmpty())
        return;
    emit saveRequested(title);
    accept();
}
