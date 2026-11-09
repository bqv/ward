#pragma once

#include <QDialog>
#include <QVariantMap>

class ClientFacade;
class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;

class WorkspacePickerDialog : public QDialog
{
    Q_OBJECT

public:
    WorkspacePickerDialog(ClientFacade *facade, const QString &iconDirectory,
                          QWidget *parent = nullptr);

    void showPicker();

private:
    void applyListing();
    void rebuildCrumbs(const QVariantMap &listing);
    void rebuildEntries(const QVariantMap &listing);
    void refreshState();
    void submitDirectory();
    void addCurrentDirectory();

    ClientFacade *m_facade = nullptr;
    QString m_iconDirectory;

    QScrollArea *m_crumbScroll = nullptr;
    QWidget *m_crumbHost = nullptr;
    QHBoxLayout *m_crumbLayout = nullptr;
    QListWidget *m_entries = nullptr;
    QLineEdit *m_name = nullptr;
    QPushButton *m_create = nullptr;
    QPushButton *m_add = nullptr;
    QCheckBox *m_showHidden = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_truncated = nullptr;
    QLabel *m_notice = nullptr;

    QString m_path;
    int m_visibleCount = 0;
    bool m_loading = false;
};
