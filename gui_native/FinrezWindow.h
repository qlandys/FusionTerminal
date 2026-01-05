#pragma once

#include <QDialog>
#include "ConnectionStore.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class TradeManager;

class FinrezWindow final : public QDialog {
    Q_OBJECT

public:
    explicit FinrezWindow(TradeManager *manager, QWidget *parent = nullptr);

    void refreshUi();

private:
    QString profileTitle(ConnectionStore::Profile profile) const;
    QString profileTitleWithTag(ConnectionStore::Profile profile) const;
    QColor profileStateColor(ConnectionStore::Profile profile) const;
    ConnectionStore::Profile currentProfile() const;

    void rebuildTable(ConnectionStore::Profile profile);

    TradeManager *m_manager = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QLabel *m_statusDot = nullptr;
    QLabel *m_profileLabel = nullptr;
    QPushButton *m_resetBtn = nullptr;
    QTableWidget *m_table = nullptr;
};

