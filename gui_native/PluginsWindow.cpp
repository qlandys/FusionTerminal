#include "PluginsWindow.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

PluginsWindow::PluginsWindow(QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget(this))
{
    setWindowTitle(tr("Fusion Terminal - Mods"));
    setModal(false);
    resize(620, 420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("Installed mods"), this);
    layout->addWidget(title);

    layout->addWidget(m_list, 1);

    auto *hint = new QLabel(
        tr("Python-based mods will appear here.\n"
           "For now this is a placeholder window."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    layout->addWidget(closeButton);
}
