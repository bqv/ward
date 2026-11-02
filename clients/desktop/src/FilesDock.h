#pragma once

#include <QWidget>

class ClientFacade;
class QLineEdit;
class QPaintEvent;
class QPlainTextEdit;

class FilesDock : public QWidget
{
    Q_OBJECT

public:
    explicit FilesDock(ClientFacade *facade, QWidget *parent = nullptr);
    ~FilesDock() override;

    bool shown() const { return m_shown; }
    void setShown(bool shown);

    int expandedWidth() const { return m_expandedWidth; }
    void setExpandedWidth(int width);

    int dockWidth() const;

    bool available() const;

signals:
    void shownChanged();
    void dockWidthChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    class Header;
    class Tree;
    class Line;

    void refresh();
    void refreshViewer();
    void refreshFooter();
    void loadRootIfNeeded();
    void applyWidth();

    ClientFacade *m_facade = nullptr;
    Header *m_header = nullptr;
    QWidget *m_breadcrumb = nullptr;
    QWidget *m_up = nullptr;
    QLineEdit *m_path = nullptr;
    Tree *m_tree = nullptr;
    QPlainTextEdit *m_viewer = nullptr;
    Line *m_footer = nullptr;

    bool m_shown = false;
    int m_expandedWidth = 320;
};
