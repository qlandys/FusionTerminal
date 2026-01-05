#pragma once

#include <QDialog>
#include "TradeTypes.h"

class TradeManager;
class QComboBox;
class QLineEdit;
class QLabel;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTableView;

class TradesWindow : public QDialog {
    Q_OBJECT
public:
    explicit TradesWindow(TradeManager *manager, QWidget *parent = nullptr);

    void refreshUi();

private:
    void appendTrade(const ExecutedTrade &trade);
    void rebuildAccountFilter();

    TradeManager *m_manager = nullptr;
    QComboBox *m_accountFilter = nullptr;
    QLineEdit *m_symbolFilter = nullptr;
    QComboBox *m_sideFilter = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
};

