#include "TradesWindow.h"
#include "TradeManager.h"

#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QSet>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString sideText(OrderSide side)
{
    return side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL");
}

class TradesFilterProxy final : public QSortFilterProxyModel {
public:
    TradesFilterProxy(QObject *parent,
                      QComboBox *account,
                      QLineEdit *symbol,
                      QComboBox *side)
        : QSortFilterProxyModel(parent)
        , m_account(account)
        , m_symbol(symbol)
        , m_side(side)
    {
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (!sourceModel()) {
            return true;
        }

        const QString accountFilter = m_account ? m_account->currentText().trimmed() : QString();
        if (!accountFilter.isEmpty() && accountFilter != QStringLiteral("All")) {
            const QString v = sourceModel()
                                  ->index(sourceRow, 0, sourceParent)
                                  .data(Qt::DisplayRole)
                                  .toString();
            if (v != accountFilter) {
                return false;
            }
        }

        const QString symFilter = m_symbol ? m_symbol->text().trimmed() : QString();
        if (!symFilter.isEmpty()) {
            const QString v = sourceModel()
                                  ->index(sourceRow, 1, sourceParent)
                                  .data(Qt::DisplayRole)
                                  .toString();
            if (!v.contains(symFilter, Qt::CaseInsensitive)) {
                return false;
            }
        }

        const QString sideFilter = m_side ? m_side->currentText().trimmed() : QString();
        if (!sideFilter.isEmpty() && sideFilter != QStringLiteral("All")) {
            const QString v = sourceModel()
                                  ->index(sourceRow, 2, sourceParent)
                                  .data(Qt::DisplayRole)
                                  .toString();
            if (v != sideFilter) {
                return false;
            }
        }

        return true;
    }

private:
    QComboBox *m_account = nullptr;
    QLineEdit *m_symbol = nullptr;
    QComboBox *m_side = nullptr;
};
} // namespace

TradesWindow::TradesWindow(TradeManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("Trades"));
    setMinimumSize(920, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *filters = new QHBoxLayout();
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(8);

    m_accountFilter = new QComboBox(this);
    m_accountFilter->addItem(QStringLiteral("All"));
    m_accountFilter->setMinimumWidth(160);
    filters->addWidget(new QLabel(tr("Account"), this));
    filters->addWidget(m_accountFilter);

    m_symbolFilter = new QLineEdit(this);
    m_symbolFilter->setPlaceholderText(tr("Ticker"));
    m_symbolFilter->setClearButtonEnabled(true);
    filters->addWidget(new QLabel(tr("Symbol"), this));
    filters->addWidget(m_symbolFilter, 1);

    m_sideFilter = new QComboBox(this);
    m_sideFilter->addItems({QStringLiteral("All"), QStringLiteral("BUY"), QStringLiteral("SELL")});
    filters->addWidget(new QLabel(tr("Side"), this));
    filters->addWidget(m_sideFilter);

    auto *clearBtn = new QPushButton(tr("Clear"), this);
    filters->addWidget(clearBtn);

    root->addLayout(filters);

    m_model = new QStandardItemModel(this);
    m_model->setColumnCount(10);
    m_model->setHorizontalHeaderLabels(
        {tr("Account"),
         tr("Symbol"),
         tr("Side"),
         tr("Qty"),
         tr("USD"),
         tr("Price"),
         tr("P&L %"),
         tr("P&L $"),
         tr("Fee"),
         tr("Time")});

    m_proxy = new TradesFilterProxy(this, m_accountFilter, m_symbolFilter, m_sideFilter);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_table = new QTableView(this);
    m_table->setModel(m_proxy);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    root->addWidget(m_table, 1);

    m_statusLabel = new QLabel(this);
    root->addWidget(m_statusLabel);

    auto applyFilters = [this]() {
        if (m_proxy) {
            m_proxy->invalidate();
        }
        if (m_statusLabel) {
            m_statusLabel->setText(tr("Rows: %1").arg(m_proxy ? m_proxy->rowCount() : 0));
        }
    };

    connect(m_accountFilter, &QComboBox::currentTextChanged, this, [applyFilters]() { applyFilters(); });
    connect(m_sideFilter, &QComboBox::currentTextChanged, this, [applyFilters]() { applyFilters(); });
    connect(m_symbolFilter, &QLineEdit::textChanged, this, [applyFilters]() { applyFilters(); });

    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_manager) {
            m_manager->clearExecutedTrades();
        }
        refreshUi();
    });

    if (m_manager) {
        connect(m_manager, &TradeManager::tradeExecuted, this, [this](const ExecutedTrade &t) {
            appendTrade(t);
            rebuildAccountFilter();
        });
        connect(m_manager, &TradeManager::tradeHistoryCleared, this, [this]() { refreshUi(); });
    }

    refreshUi();
}

void TradesWindow::refreshUi()
{
    if (!m_model) {
        return;
    }
    m_model->removeRows(0, m_model->rowCount());
    if (m_manager) {
        const auto trades = m_manager->executedTrades();
        for (const auto &t : trades) {
            appendTrade(t);
        }
    }
    rebuildAccountFilter();
    if (m_proxy) {
    m_proxy->sort(9, Qt::DescendingOrder);
        m_proxy->invalidate();
    }
    if (m_statusLabel) {
        m_statusLabel->setText(tr("Rows: %1").arg(m_proxy ? m_proxy->rowCount() : 0));
    }
}

void TradesWindow::appendTrade(const ExecutedTrade &trade)
{
    if (!m_model) {
        return;
    }

    const qint64 t = trade.timeMs > 0 ? trade.timeMs : QDateTime::currentMSecsSinceEpoch();
    const QString timeText =
        QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    const double usd = trade.price > 0.0 && trade.quantity > 0.0 ? trade.price * trade.quantity : 0.0;
    const QString feeText =
        trade.feeAmount > 0.0
            ? QStringLiteral("%1 %2")
                  .arg(QString::number(trade.feeAmount, 'f', 8),
                       trade.feeCurrency.isEmpty() ? QStringLiteral("-") : trade.feeCurrency)
            : QStringLiteral("-");

    QList<QStandardItem *> row;
    row << new QStandardItem(trade.accountName);
    row << new QStandardItem(trade.symbol);

    auto *sideItem = new QStandardItem(sideText(trade.side));
    sideItem->setForeground(trade.side == OrderSide::Buy ? QColor("#2e7d32") : QColor("#c62828"));
    row << sideItem;

    row << new QStandardItem(QString::number(trade.quantity, 'f', 8));
    row << new QStandardItem(QString::number(usd, 'f', 4));
    row << new QStandardItem(QString::number(trade.price, 'f', 8));

    const QString pnlPctText =
        (std::abs(trade.realizedPct) > 1e-12)
            ? QStringLiteral("%1%2%")
                  .arg(trade.realizedPct >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
                  .arg(QString::number(std::abs(trade.realizedPct), 'f', std::abs(trade.realizedPct) >= 1.0 ? 2 : 3))
            : QStringLiteral("0.0%");
    row << new QStandardItem(pnlPctText);

    auto *pnlItem = new QStandardItem(
        QStringLiteral("%1%2$")
            .arg(trade.realizedPnl >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
            .arg(QString::number(std::abs(trade.realizedPnl), 'f', std::abs(trade.realizedPnl) >= 1.0 ? 2 : 4)));
    if (std::abs(trade.realizedPnl) < 1e-12) {
        pnlItem->setText(QStringLiteral("0$"));
    }
    const QColor pnlBg = trade.realizedPnl >= 0.0 ? QColor("#7CFC90") : QColor("#ff9ea3");
    pnlItem->setBackground(pnlBg);
    pnlItem->setForeground(QColor("#000000"));
    row << pnlItem;

    row << new QStandardItem(feeText);

    auto *timeItem = new QStandardItem(timeText);
    timeItem->setData(t, Qt::UserRole);
    row << timeItem;

    m_model->appendRow(row);
}

void TradesWindow::rebuildAccountFilter()
{
    if (!m_accountFilter || !m_model) {
        return;
    }
    const QString current = m_accountFilter->currentText();

    QSet<QString> accounts;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        accounts.insert(m_model->item(r, 0)->text());
    }

    QStringList items = accounts.values();
    std::sort(items.begin(), items.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    m_accountFilter->blockSignals(true);
    m_accountFilter->clear();
    m_accountFilter->addItem(QStringLiteral("All"));
    for (const auto &a : items) {
        if (!a.trimmed().isEmpty()) {
            m_accountFilter->addItem(a);
        }
    }
    const int idx = m_accountFilter->findText(current);
    if (idx >= 0) {
        m_accountFilter->setCurrentIndex(idx);
    }
    m_accountFilter->blockSignals(false);
}
