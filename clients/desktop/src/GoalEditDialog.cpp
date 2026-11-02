#include "GoalEditDialog.h"

#include "ShellChrome.h"
#include "Theme.h"

#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextCursor>
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

GoalEditDialog::GoalEditDialog(QWidget *parent)
    : QDialog(parent)
{
    Theme *theme = Theme::instance();

    setObjectName(QStringLiteral("goalEditDialog"));
    setWindowTitle(QStringLiteral("Edit goal"));
    setWindowModality(Qt::WindowModal);
    setPalette(Theme::palette());
    setFont(Chrome::uiFont(theme->fontUI()));
    resize(520, 340);
    setMinimumSize(420, 260);
    setSizeGripEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme->space6(), theme->space6(),
                               theme->space6(), theme->space6());
    layout->setSpacing(theme->space5());

    layout->addWidget(textLabel(this, QStringLiteral("Edit goal"), theme->fg(),
                                Chrome::uiFont(theme->fontProse() + 2, true)));
    layout->addWidget(textLabel(this,
                                QStringLiteral("Change the objective or the number of rounds "
                                               "this goal may run."),
                                theme->dim(), Chrome::uiFont(theme->fontData())));

    layout->addWidget(textLabel(this, QStringLiteral("Objective"), theme->caption(),
                                Chrome::monoFont(theme->fontData())));

    m_objective = new QPlainTextEdit(this);
    m_objective->setObjectName(QStringLiteral("goalObjective"));
    m_objective->setFont(Chrome::uiFont(theme->fontUI()));
    m_objective->setTabChangesFocus(true);
    m_objective->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_objective->setFrameShape(QFrame::NoFrame);
    m_objective->setMinimumHeight(4 * theme->rowHeight());
    Chrome::styleScrollBar(m_objective->verticalScrollBar());
    QPalette objectivePalette = m_objective->palette();
    objectivePalette.setColor(QPalette::Base, theme->fieldFill());
    objectivePalette.setColor(QPalette::Text, theme->fg());
    objectivePalette.setColor(QPalette::Highlight, theme->accent());
    objectivePalette.setColor(QPalette::HighlightedText, theme->accentFg());
    m_objective->setPalette(objectivePalette);
    m_objective->viewport()->setPalette(objectivePalette);
    layout->addWidget(m_objective, 1);

    auto *rounds = new QHBoxLayout();
    rounds->setContentsMargins(0, 0, 0, 0);
    rounds->setSpacing(theme->space3());
    rounds->addWidget(textLabel(this, QStringLiteral("Rounds"), theme->caption(),
                                Chrome::monoFont(theme->fontData())));
    m_rounds = new QSpinBox(this);
    m_rounds->setObjectName(QStringLiteral("goalRounds"));
    m_rounds->setRange(1, 999);
    m_rounds->setFont(Chrome::uiFont(theme->fontUI()));
    m_rounds->setMinimumWidth(6 * theme->space6());
    QPalette roundsPalette = m_rounds->palette();
    roundsPalette.setColor(QPalette::Base, theme->fieldFill());
    roundsPalette.setColor(QPalette::Text, theme->fg());
    roundsPalette.setColor(QPalette::ButtonText, theme->fg());
    m_rounds->setPalette(roundsPalette);
    rounds->addWidget(m_rounds);
    rounds->addStretch(1);
    layout->addLayout(rounds);

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
    connect(m_save, &QPushButton::clicked, this, &GoalEditDialog::save);
    connect(m_objective, &QPlainTextEdit::textChanged, this, [this]() {
        m_save->setEnabled(!m_objective->toPlainText().trimmed().isEmpty());
    });
}

void GoalEditDialog::showDialog(const QString &objective, int maxGoalRounds)
{
    m_objective->setPlainText(objective);
    m_objective->moveCursor(QTextCursor::End);
    m_rounds->setValue(maxGoalRounds > 0 ? maxGoalRounds : 1);
    m_save->setEnabled(!m_objective->toPlainText().trimmed().isEmpty());
    m_objective->setFocus();
    open();
    raise();
    activateWindow();
}

void GoalEditDialog::save()
{
    emit saveRequested(m_objective->toPlainText(), m_rounds->value());
    accept();
}
