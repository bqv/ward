#pragma once

#include <QString>
#include <QVariantList>
#include <QWidget>

class PendingBand : public QWidget
{
    Q_OBJECT

public:
    explicit PendingBand(QWidget *parent = nullptr);

    bool approvalShown() const;
    bool questionShown() const;
    int dockHeight() const;

    void showApproval(const QString &requestId, const QString &toolName, const QString &detail);
    void dismissApproval(const QString &requestId);
    void showQuestions(const QString &eventId, const QVariantList &questions);
    void dismissQuestions(const QString &eventId);

signals:
    void approvalAnswered(const QString &requestId, bool allow);
    void questionsAnswered(const QString &eventId, const QVariantList &answers);
    void dockHeightChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    class ApprovalCard;
    class QuestionCard;

    void syncDockHeight();

    ApprovalCard *m_approval = nullptr;
    QuestionCard *m_question = nullptr;
};
