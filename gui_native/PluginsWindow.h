#pragma once

#include <QDialog>

class QListWidget;

class PluginsWindow : public QDialog {
    Q_OBJECT

public:
    explicit PluginsWindow(QWidget *parent = nullptr);

private:
    QListWidget *m_list;
};

