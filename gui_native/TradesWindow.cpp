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
#include <cmath>

namespace {
QString dirText(bool isLong)
{
    return isLong ? QStringLiteral("LONG") : QStringLiteral("SHORT");
}

QString formatFeeMap(const QHash<QString, double> &byCcy)
{
    QStringList parts;
    parts.reserve(byCcy.size());
    for (auto it = byCcy.constBegin(); it != byCcy.constEnd(); ++it) {
        const QString ccy = it.key().trimmed().isEmpty() ? QStringLiteral("-") : it.key().trimmed();
        const double v = it.value();
        if (std::abs(v) <= 1e-12) {
            continue;
        }
        parts.push_back(QStringLiteral("-%1 %2").arg(QString::number(std::abs(v), 'f', 4), ccy));
    }
    if (parts.isEmpty()) {
        return QStringLiteral("-");
    }
    return parts.join(QStringLiteral(" + "));
}

struct ClosedTrade {
    QString account;
    QString symbol;
    bool isLong = true;
    bool isFutures = false;
    int leverage = 0;
    double qty = 0.0; // base qty closed
    double entryNotional = 0.0;
    double exitNotional = 0.0;
    double entryPrice = 0.0;
    double exitPrice = 0.0;
    double pnl = 0.0;
    double pnlPct = 0.0;
    QHash<QString, double> feesByCcy;
    qint64 openedMs = 0;
    qint64 closedMs = 0;
};

QVector<ClosedTrade> buildClosedTrades(QVector<ExecutedTrade> trades)
{
    std::sort(trades.begin(), trades.end(), [](const ExecutedTrade &a, const ExecutedTrade &b) {
        return a.timeMs < b.timeMs;
    });

    struct State {
        bool active = false;
        double posSignedQty = 0.0;
        double posAvg = 0.0;
        bool isLong = true;
        bool isFutures = false;
        int leverage = 0;
        double closedQty = 0.0;
        double entryNotionalClosed = 0.0;
        double exitNotionalClosed = 0.0;
        double pnl = 0.0;
        QHash<QString, double> feesByCcy;
        qint64 openedMs = 0;
        qint64 closedMs = 0;
    };

    QHash<QString, State> stByKey;
    QVector<ClosedTrade> out;

    auto finalize = [&](const QString &account, const QString &symbol, State &st) {
        if (!(st.closedQty > 0.0) || !(st.entryNotionalClosed > 0.0) || st.closedMs <= 0) {
            st = State{};
            return;
        }
        ClosedTrade ct;
        ct.account = account;
        ct.symbol = symbol;
        ct.isLong = st.isLong;
        ct.isFutures = st.isFutures;
        ct.leverage = st.leverage;
        ct.qty = st.closedQty;
        ct.entryNotional = st.entryNotionalClosed;
        ct.exitNotional = st.exitNotionalClosed;
        ct.entryPrice = st.entryNotionalClosed / st.closedQty;
        ct.exitPrice = st.exitNotionalClosed / st.closedQty;
        ct.pnl = st.pnl;
        ct.pnlPct = (st.entryNotionalClosed > 0.0) ? (st.pnl / st.entryNotionalClosed) * 100.0 : 0.0;
        ct.feesByCcy = st.feesByCcy;
        ct.openedMs = st.openedMs;
        ct.closedMs = st.closedMs;
        out.push_back(ct);
        st = State{};
    };

    for (const auto &t : trades) {
        const QString account = t.accountName.trimmed();
        const QString sym = t.symbol.trimmed().toUpper();
        if (account.isEmpty() || sym.isEmpty() || !(t.price > 0.0) || !(t.quantity > 0.0)) {
            continue;
        }
        const QString key = account + QStringLiteral("|") + sym;
        State &st = stByKey[key];

        const int sideSign = (t.side == OrderSide::Buy) ? +1 : -1;
        double qty = t.quantity;

        if (std::abs(t.feeAmount) > 1e-12) {
            const QString ccy = t.feeCurrency.trimmed().toUpper();
            st.feesByCcy[ccy] += std::abs(t.feeAmount);
        }
        if (t.isFutures) {
            st.isFutures = true;
        }
        if (t.leverage > 0) {
            st.leverage = t.leverage;
        }

        const qint64 timeMs = t.timeMs > 0 ? t.timeMs : QDateTime::currentMSecsSinceEpoch();

        if (!st.active || std::abs(st.posSignedQty) <= 1e-12) {
            st.active = true;
            st.isLong = (sideSign > 0);
            st.posSignedQty = sideSign * qty;
            st.posAvg = t.price;
            st.openedMs = timeMs;
            continue;
        }

        const int posSign = (st.posSignedQty > 0) ? +1 : -1;
        if (posSign == sideSign) {
            const double oldAbs = std::abs(st.posSignedQty);
            const double newAbs = oldAbs + qty;
            st.posAvg = (oldAbs * st.posAvg + qty * t.price) / std::max(1e-12, newAbs);
            st.posSignedQty += sideSign * qty;
            continue;
        }

        // Closing / reducing (or reversal).
        double absPos = std::abs(st.posSignedQty);
        const double closeQty = std::min(qty, absPos);
        st.closedQty += closeQty;
        st.entryNotionalClosed += closeQty * st.posAvg;
        st.exitNotionalClosed += closeQty * t.price;
        st.pnl += t.realizedPnl;
        st.posSignedQty += sideSign * closeQty;
        st.closedMs = timeMs;

        qty -= closeQty;
        if (qty > 1e-12) {
            // Reversal: finalize the previous trade, start a new one with the remainder.
            finalize(account, sym, st);

            State &st2 = stByKey[key];
            st2.active = true;
            st2.isLong = (sideSign > 0);
            st2.posSignedQty = sideSign * qty;
            st2.posAvg = t.price;
            st2.openedMs = timeMs;
            if (std::abs(t.feeAmount) > 1e-12) {
                const QString ccy = t.feeCurrency.trimmed().toUpper();
                st2.feesByCcy[ccy] += std::abs(t.feeAmount);
            }
            st2.isFutures = t.isFutures;
            st2.leverage = t.leverage;
            continue;
        }

        if (std::abs(st.posSignedQty) <= 1e-12) {
            finalize(account, sym, st);
        }
    }

    return out;
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
    m_sideFilter->addItems({QStringLiteral("All"), QStringLiteral("LONG"), QStringLiteral("SHORT")});
    filters->addWidget(new QLabel(tr("Side"), this));
    filters->addWidget(m_sideFilter);

    auto *clearBtn = new QPushButton(tr("Clear"), this);
    filters->addWidget(clearBtn);

    root->addLayout(filters);

    m_model = new QStandardItemModel(this);
    m_model->setColumnCount(12);
    m_model->setHorizontalHeaderLabels(
        {tr("Account"),
         tr("Symbol"),
         tr("Side"),
         tr("Qty"),
         tr("USD"),
         tr("Entry"),
         tr("Exit"),
         tr("Lev"),
         tr("Fee"),
         tr("P&L %"),
         tr("P&L $"),
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
            Q_UNUSED(t);
            refreshUi();
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
        const auto closed = buildClosedTrades(m_manager->executedTrades());
        for (const auto &c : closed) {
            QVariantMap row;
            row.insert(QStringLiteral("acct"), c.account);
            row.insert(QStringLiteral("sym"), c.symbol);
            row.insert(QStringLiteral("long"), c.isLong);
            row.insert(QStringLiteral("qty"), c.qty);
            row.insert(QStringLiteral("usd"), c.entryNotional);
            row.insert(QStringLiteral("entry"), c.entryPrice);
            row.insert(QStringLiteral("exit"), c.exitPrice);
            row.insert(QStringLiteral("lev"), c.leverage);
            row.insert(QStringLiteral("feeText"), formatFeeMap(c.feesByCcy));
            row.insert(QStringLiteral("pnl"), c.pnl);
            row.insert(QStringLiteral("pnlPct"), c.pnlPct);
            row.insert(QStringLiteral("timeMs"), c.closedMs);
            appendClosedTradeRow(row);
        }
    }
    rebuildAccountFilter();
    if (m_proxy) {
        m_proxy->sort(11, Qt::DescendingOrder);
        m_proxy->invalidate();
    }
    if (m_statusLabel) {
        m_statusLabel->setText(tr("Rows: %1").arg(m_proxy ? m_proxy->rowCount() : 0));
    }
}

void TradesWindow::appendClosedTradeRow(const QVariantMap &trade)
{
    if (!m_model) {
        return;
    }

    const qint64 t = trade.value(QStringLiteral("timeMs")).toLongLong();
    const QString timeText =
        QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    const double usd = trade.value(QStringLiteral("usd")).toDouble();
    const bool isLong = trade.value(QStringLiteral("long")).toBool();
    const int lev = trade.value(QStringLiteral("lev")).toInt();
    const QString levText = lev > 0 ? QStringLiteral("%1x").arg(lev) : QStringLiteral("-");

    QList<QStandardItem *> row;
    row << new QStandardItem(trade.value(QStringLiteral("acct")).toString());
    row << new QStandardItem(trade.value(QStringLiteral("sym")).toString());

    auto *sideItem = new QStandardItem(dirText(isLong));
    sideItem->setForeground(isLong ? QColor("#2e7d32") : QColor("#c62828"));
    row << sideItem;

    row << new QStandardItem(QString::number(trade.value(QStringLiteral("qty")).toDouble(), 'f', 8));
    row << new QStandardItem(QString::number(usd, 'f', 4));
    row << new QStandardItem(QString::number(trade.value(QStringLiteral("entry")).toDouble(), 'f', 8));
    row << new QStandardItem(QString::number(trade.value(QStringLiteral("exit")).toDouble(), 'f', 8));
    row << new QStandardItem(levText);
    row << new QStandardItem(trade.value(QStringLiteral("feeText")).toString());

    const double pnlPct = trade.value(QStringLiteral("pnlPct")).toDouble();
    const QString pnlPctText =
        (std::abs(pnlPct) > 1e-12)
            ? QStringLiteral("%1%2%")
                  .arg(pnlPct >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
                  .arg(QString::number(std::abs(pnlPct), 'f', std::abs(pnlPct) >= 1.0 ? 2 : 3))
            : QStringLiteral("0.0%");
    row << new QStandardItem(pnlPctText);

    const double pnl = trade.value(QStringLiteral("pnl")).toDouble();
    auto *pnlItem = new QStandardItem(
        QStringLiteral("%1%2$")
            .arg(pnl >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
            .arg(QString::number(std::abs(pnl), 'f', std::abs(pnl) >= 1.0 ? 2 : 4)));
    if (std::abs(pnl) < 1e-12) {
        pnlItem->setText(QStringLiteral("0$"));
    }
    const QColor pnlBg = pnl >= 0.0 ? QColor("#7CFC90") : QColor("#ff9ea3");
    pnlItem->setBackground(pnlBg);
    pnlItem->setForeground(QColor("#000000"));
    row << pnlItem;

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
